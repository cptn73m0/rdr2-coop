#include "coop.h"
#include "config.h"
#include "ghost_frame.h"
#include "log.h"
#include "natives.h"
#include "net/client.h"
#include "persist.h"
#include "world_scan.h"
#include "world_sync.h"
#include "mission_catalog.h"
#include "mission_rewards.h"
#include "mission_barrier_rules.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace coop {

namespace {

// Ghost ped (the remote friend's player model). Rendered via the
// mission+variation+visible setup. Its state later comes from a network
// GhostFrame stream; for now we EMULATE a remote player joining ~60s after
// the local player enters, to prove spawn + "Player joined" subtitle, and
// to prove the ped is NOT tied to the local player's movement.
constexpr unsigned long long GHOST_PED_MODEL = 0xCF7E73BEull; // a_m_m_armtownfolk_01
constexpr float SPAWN_DISTANCE               = 3.0f;
constexpr int   JOIN_DELAY_TICKS             = 60 * 60; // ~60s at 60 ticks/s

unsigned int s_ghost = 0;
unsigned int s_blip = 0;
bool s_ghostDead = false;
bool s_localDiedNotified = false; // отправили ли серверу Died для своего педа
unsigned long long s_requestedModel = 0; // какая модель уже запрошена у движка
unsigned long long s_ignoredPedModelHash = 0; // невалидный хэш из сети (лог один раз)
bool s_modelLoadedLogged = false;
bool s_playerJoinedNotified = false;
unsigned int s_tickCount = 0;
char s_joinedName[64] = "";
unsigned int s_subtitleTicksLeft = 0;   // remaining ticks the subtitle is shown

// --- Emulated join notification (kept as offline fallback) ---
bool CheckJoinEvent(char (&outName)[64])
{
    // After the local player is in the world and ~60s passed, emulate a
    // remote player connecting.
    if (s_tickCount >= JOIN_DELAY_TICKS && !s_playerJoinedNotified)
    {
        std::snprintf(outName, sizeof(outName), "Bob");
        return true;
    }
    return false;
}

// --- Native helpers -----------------------------------------------------
unsigned int GetPlayerPedFor(unsigned int player)
{
    using namespace natives_api;
    Init(natives::GET_PLAYER_PED);
    Push(player);
    return static_cast<unsigned int>(Call());
}

float GetEntityHeading(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_HEADING);
    Push(handle);
    return CallFloat();
}

Vec3 GetEntityCoords(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_COORDS);
    Push(handle);
    Push(false);
    Push(true);
    Vec3 out{0, 0, 0};
    CallVec3(out);
    return out;
}

bool HasModelLoaded(unsigned long long model)
{
    using namespace natives_api;
    Init(natives::HAS_MODEL_LOADED);
    Push(model);
    return Call() != 0;
}

// IS_MODEL_VALID — проверка хэша модели, пришедшего из сети. Раньше битый
// model hash приводил к DeleteGhostEntity + RequestModel(невалидной модели):
// HasModelLoaded() навсегда false, Tick() ранний return — и переставали
// обрабатываться кадры/наряд/выстрелы/connectionLost.
bool IsModelHashValid(unsigned long long model)
{
    if (!model) return false;
    using namespace natives_api;
    Init(natives::IS_MODEL_VALID);
    Push(model);
    return Call() != 0;
}

void RequestModel(unsigned long long model)
{
    // Центральный предохранитель: невалидный хэш не запрашиваем вообще.
    if (!IsModelHashValid(model)) return;
    using namespace natives_api;
    Init(natives::REQUEST_MODEL);
    Push(model);
    Push(static_cast<unsigned long long>(0)); // p1: 2-й аргумент обязателен
    Call();
}

bool DoesEntityExist(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::DOES_ENTITY_EXIST);
    Push(handle);
    return Call() != 0;
}

bool IsPedDeadOrDying(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_DEAD_OR_DYING);
    Push(handle);
    return Call() != 0;
}

void SetEntityVisibleForce(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_VISIBLE);
    Push(handle);
    Push(true);
    Call();
}

void SetEntityMission(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
    Push(handle);
    Push(true);
    Push(true);
    Call();
    // Бессмертие: пед-призрак не должен умирать от коллизий/бандитов/физики.
    // Он умрёт только когда умрёт оригинал игрока (сервер пришлёт Died).
    Init(natives::SET_ENTITY_INVINCIBLE);
    Push(handle);
    Push(static_cast<unsigned long long>(1));
    Call();
}

void RandomizePedComponents(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_PED_RANDOM_COMPONENT_VARIATION);
    Push(handle);
    Push(static_cast<unsigned long long>(0));
    Call();
}

void TaskStandStill(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::TASK_STAND_STILL);
    Push(handle);
    Push(-1);
    Call();
}

void PlaceOnGround(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::PLACE_OBJECT_ON_GROUND_PROPERLY);
    Push(handle);
    Call();
}

// GET_GROUND_Z_FOR_3D_COORD(x,y,z,float* groundZ,BOOL p4) — 5 аргументов.
// Единая обёртка: раньше во всех вызовах передавали 4, и движок читал p4 из
// мусора буфера invoker'а (запрос земли нестабилен: спавн/лошадь под землёй).
bool GetGroundZForCoord(const Vec3& at, float& outZ)
{
    using namespace natives_api;
    Init(natives::GET_GROUND_Z_FOR_3D_COORD);
    Push(at.x);
    Push(at.y);
    Push(at.z);
    outZ = 0.f;
    Push(reinterpret_cast<unsigned long long>(&outZ));
    Push(static_cast<unsigned long long>(0)); // p4
    return Call() != 0;
}

void SetEntityCoords(unsigned int handle, const Vec3& v)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_COORDS);
    Push(handle);
    Push(v.x);
    Push(v.y);
    Push(v.z);
    Push(false);
    Push(false);
    Push(false);
    Push(false); // clearTasks=false: НЕ сбрасывать задачи (TASK_GO/CLIMB и т.п.),
                // иначе ручное перемещение на ходу обрывает анимацию (Т-поза).
    Call();
}

void SetEntityHeading(unsigned int handle, float heading)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_HEADING);
    Push(handle);
    Push(heading);
    Call();
}

unsigned int CreateGhostPed(unsigned long long model, const Vec3& at, float heading)
{
    using namespace natives_api;
    Init(natives::CREATE_PED);
    Push(model);
    Push(at.x);
    Push(at.y);
    Push(at.z);
    Push(heading);
    Push(false);
    Push(false);
    Push(false);
    Push(false);
    return static_cast<unsigned int>(Call());
}

// Draw a small subtitle-like line (compact, lower-left). Text-drawing must
// run within a ScriptWait loop, which Tick() is. IMPORTANT: RDR2 requires
// the string to be prepared with MISC::VAR_STRING(10, "LITERAL_STRING", text)
// before _BG_DISPLAY_TEXT, otherwise nothing is drawn.
void DrawSubtitle(const char* text)
{
    const float sx = 0.15f; // left-ish
    const float sy = 0.85f; // near bottom

    // Prepare the string handle via VAR_STRING.
    unsigned long long strHandle = 0;
    {
        using namespace natives_api;
        Init(natives::MISC_VAR_STRING);
        Push(static_cast<unsigned long long>(10));          // forced string type
        Push(reinterpret_cast<unsigned long long>(const_cast<char*>("LITERAL_STRING")));
        Push(reinterpret_cast<unsigned long long>(const_cast<char*>(text)));
        strHandle = Call();
    }

    using namespace natives_api;
    Init(natives::UI_BG_SET_TEXT_SCALE);
    Push(0.32f); Push(0.32f); // как у игровых субтитров — не выпирает
    Call();
    Init(natives::UI_BG_SET_TEXT_COLOR);
    Push(215); Push(215); Push(215); Push(235); // мягкий белый
    Call();
    Init(natives::UI_BG_DISPLAY_TEXT);
    Push(strHandle);
    Push(sx);
    Push(sy);
    Call();
}

// Маленькая строка-индикатор в левом верхнем углу (для отладки сети).
void DrawHudLine(const char* text)
{
    unsigned long long strHandle = 0;
    {
        using namespace natives_api;
        Init(natives::MISC_VAR_STRING);
        Push(static_cast<unsigned long long>(10));
        Push(reinterpret_cast<unsigned long long>(const_cast<char*>("LITERAL_STRING")));
        Push(reinterpret_cast<unsigned long long>(const_cast<char*>(text)));
        strHandle = Call();
    }
    using namespace natives_api;
    Init(natives::UI_BG_SET_TEXT_SCALE);
    Push(0.28f); Push(0.28f);
    Call();
    Init(natives::UI_BG_SET_TEXT_COLOR);
    Push(200); Push(200); Push(200); Push(210); // приглушённый
    Call();
    Init(natives::UI_BG_DISPLAY_TEXT);
    Push(strHandle);
    Push(0.02f); // x
    Push(0.03f); // y
    Call();
}

// --- Blip (map/minimap) handling --------------------------------------
// BLIP_ADD_FOR_ENTITY returns 0 in this build, so we create the blip from
// the ped's world coords and keep its coords up to date each tick.
unsigned int CreateFriendlyBlipAt(const Vec3& at)
{
    using namespace natives_api;
    Init(natives::MAP_BLIP_ADD_FOR_COORDS);
    // A plain, valid base sprite (BLIP_ADD_FOR_COORDS expects a sprite,
    // not a style). The friendly modifier below colors it yellow.
    Push(static_cast<unsigned long long>(natives::BLIP_SPRITE_WHITE_DOT));
    Push(at.x);
    Push(at.y);
    Push(at.z);
    unsigned int blip = static_cast<unsigned int>(Call());
    if (blip)
    {
        // Friendly-ally marker on the map. Exact color is a cosmetic detail the
        // user does not care about; the important part is that the blip
        // appears and follows the ped.
        Init(natives::MAP_BLIP_ADD_MODIFIER);
        Push(blip);
        Push(natives::BLIP_MODIFIER_FRIENDLY);
        Call();
        Init(natives::MAP_SET_BLIP_SCALE);
        Push(blip);
        Push(0.9f);
        Call();
    }
    return blip;
}

void UpdateBlipCoords(unsigned int blip, const Vec3& at)
{
    using namespace natives_api;
    Init(natives::MAP_SET_BLIP_COORDS);
    Push(blip);
    Push(at.x);
    Push(at.y);
    Push(at.z);
    Call();
}

// Полностью убрать метку с карты/мини-карты (натив MAP_REMOVE_BLIP).
// ВАЖНО: натив принимает Blip* (указатель на handle) и зануляет его —
// передача handle значением молча НЕ удаляет метку в этом билде.
// Возвращает 0, чтобы вызывающий код сбросил s_blip.
unsigned int RemoveBlip(unsigned int blip)
{
    if (!blip) return 0;
    using namespace natives_api;
    int h = static_cast<int>(blip);
    Init(natives::MAP_REMOVE_BLIP);
    Push(reinterpret_cast<unsigned long long>(&h)); // Blip* — out-параметр
    Call();
    return 0;
}

// --- Remote player name over the ped's head ----------------------------
// Name is shown while the ped is within NAME_VISIBILITY_RANGE game meters.
constexpr float NAME_VISIBILITY_RANGE = 500.0f;
constexpr int   HEAD_BONE_ID          = 0x796E; // "SKEL_Head"

char s_pedName[64] = "";   // name currently drawn over the ped

Vec3 GetPedBoneCoords(unsigned int handle, int boneId)
{
    using namespace natives_api;
    Init(natives::GET_PED_BONE_COORDS);
    Push(handle);
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(boneId)));
    Push(0.f); Push(0.f); Push(0.f);
    Vec3 out{0, 0, 0};
    CallVec3(out);
    return out;
}

float GetDistanceBetweenCoords(const Vec3& a, const Vec3& b)
{
    using namespace natives_api;
    Init(natives::GET_DISTANCE_BETWEEN_COORDS);
    Push(a.x); Push(a.y); Push(a.z);
    Push(b.x); Push(b.y); Push(b.z);
    Push(true); // use z
    return CallFloat();
}

bool GetScreenCoordsFromWorld(const Vec3& world, float& sx, float& sy)
{
    // Signature: GET_SCREEN_COORD_FROM_WORLD_COORD(wx,wy,wz,&sx,&sy) -> bool.
    // The game writes results into the memory we pass as out-pointers, so
    // we keep two writeable float locals alive and pass their addresses.
    using namespace natives_api;
    Init(natives::GET_SCREEN_COORD_FROM_WORLD_COORD);
    Push(world.x); Push(world.y); Push(world.z);
    float sxv = 0.f, syv = 0.f;
    Push(reinterpret_cast<unsigned long long>(&sxv));
    Push(reinterpret_cast<unsigned long long>(&syv));
    unsigned long long ok = Call(); // returns bool; writes into sxv/syv
    sx = sxv;
    sy = syv;
    return ok != 0;
}

void DrawNameAt(const Vec3& worldPos, const char* name)
{
    float sx = 0.f, sy = 0.f;
    if (!GetScreenCoordsFromWorld(worldPos, sx, sy))
        return; // off-screen or behind camera
    if (sx < 0.f || sx > 1.f || sy < 0.f || sy > 1.f)
        return; // outside viewport

    // Prepare the string via VAR_STRING, then draw small centered text.
    unsigned long long strHandle = 0;
    {
        using namespace natives_api;
        Init(natives::MISC_VAR_STRING);
        Push(static_cast<unsigned long long>(10));
        Push(reinterpret_cast<unsigned long long>(const_cast<char*>("LITERAL_STRING")));
        Push(reinterpret_cast<unsigned long long>(const_cast<char*>(name)));
        strHandle = Call();
    }

    using namespace natives_api;
    Init(natives::UI_BG_SET_TEXT_SCALE);
    Push(0.28f); Push(0.28f);
    Call();
    Init(natives::UI_BG_SET_TEXT_COLOR);
    Push(235); Push(235); Push(235); Push(220); // мягкий белый вместо жёлтого
    Call();
    Init(natives::UI_BG_DISPLAY_TEXT);
    Push(strHandle);
    Push(sx);
    Push(sy);
    Call();
}

// --- Network client -----------------------------------------------------
netio::Client g_netClient;
bool g_netStarted = false;

// --- Net inbox ----------------------------------------------------------
// Единственная точка передачи данных из сетевого потока в игровой.
// Колбэки сети НЕ пишут в общие игровые переменные: они только складывают
// данные/события сюда под s_inboxMtx. Tick() забирает копию под тем же
// mutex'ом и дальше работает с локальными/игровыми копиями. Это убирает
// гонки на 88-байтном FramePayload и 400-байтном OutfitState (рваное чтение
// давало телепорты и полуприменённый наряд).
struct InboxEvents
{
    bool joinOk = false;
    bool joinIsHost = false;
    bool joinHasHeading = false;
    float joinHeading = 0.f;

    bool playerJoined = false;
    char playerJoinedName[64] = "";
    unsigned long long playerJoinedModel = 0;
    bool playerJoinedHasPos = false;
    float playerJoinedX = 0.f, playerJoinedY = 0.f, playerJoinedZ = 0.f;
    float playerJoinedHeading = 0.f;

    bool connectionLost = false;

    bool leave = false;
    char leaveName[64] = "";

    bool died = false;
    char diedName[64] = "";

    bool respawned = false;
    char respawnedName[64] = "";

    bool hostChanged = false;
    bool hostChangedIsHost = false;
};

struct NetInbox
{
    bool haveFrame = false;
    net::FramePayload frame{};
    bool haveOutfit = false;
    net::OutfitState outfit{};
    bool haveHorseOutfit = false;
    net::OutfitState horseOutfit{};
    bool haveWorldState = false;
    net::WorldState worldState{};
    InboxEvents events;
};

std::mutex s_inboxMtx;
NetInbox s_inbox;

// Копирование имени вызывается под s_inboxMtx.
void CopyInboxName(char (&dst)[64], const std::string& src)
{
    std::snprintf(dst, sizeof(dst), "%s", src.c_str());
}

bool TakeInboxFrame(net::FramePayload& out)
{
    std::lock_guard<std::mutex> lk(s_inboxMtx);
    if (!s_inbox.haveFrame) return false;
    out = s_inbox.frame;
    s_inbox.haveFrame = false;
    return true;
}

bool TakeInboxOutfit(net::OutfitState& out)
{
    std::lock_guard<std::mutex> lk(s_inboxMtx);
    if (!s_inbox.haveOutfit) return false;
    out = s_inbox.outfit;
    s_inbox.haveOutfit = false;
    return true;
}

bool TakeInboxHorseOutfit(net::OutfitState& out)
{
    std::lock_guard<std::mutex> lk(s_inboxMtx);
    if (!s_inbox.haveHorseOutfit) return false;
    out = s_inbox.horseOutfit;
    s_inbox.haveHorseOutfit = false;
    return true;
}

bool TakeInboxWorldState(net::WorldState& out)
{
    std::lock_guard<std::mutex> lk(s_inboxMtx);
    if (!s_inbox.haveWorldState) return false;
    out = s_inbox.worldState;
    s_inbox.haveWorldState = false;
    return true;
}

void TakeInboxEvents(InboxEvents& out)
{
    std::lock_guard<std::mutex> lk(s_inboxMtx);
    out = s_inbox.events;
    s_inbox.events = InboxEvents{};
}

// Состояние для спавна от сервера. Пишет/читает только игровой поток
// (сетевые колбэки складывают данные в inbox выше).
float  s_pendingHeading = 0;
char   s_pendingName[64] = "";
unsigned long long s_remoteModel = 0; // модель удалённого игрока (из PLAYER_JOINED)
// Модель, которой фактически создан текущий пед призрака (0 = ещё нет; будет
// создан/пересоздан, когда придёт модель персонажа из кадра reserved[2]).
unsigned long long s_ghostModel = 0;
// Координаты спавна педа удалённого игрока (из PLAYER_JOINED, если сервер
// прислал его последние/сохранённые координаты; иначе спавним у хоста).
bool   s_pendingHasPos = false;
float  s_pendingX = 0, s_pendingY = 0, s_pendingZ = 0;

// Последний кадр от удалённого игрока (игровой поток: забран из inbox в
// начале Tick()).
net::FramePayload s_lastFrame{};
bool s_haveFrame = false;
// Последняя известная позиция удалённого игрока — используется респавном,
// если в этом тике нет свежего кадра (частая причина телепорта: // Respawn
// раньше брал nullptr и спавнил труп на позиции входа в сессию).
Vec3  s_lastKnownPos{0.f, 0.f, 0.f};
float s_lastKnownHeading = 0.f;
bool  s_lastKnownPosValid = false;
// Не даём кадру мгновенно «воскресить» педа сразу после Died.
unsigned int s_deadGuardTicks = 0;
// Точка прицела удалённого игрока (из последнего кадра): пед целится и
// стреляет именно туда, а не «вперёд по heading».
Vec3 s_remoteAim{0.f, 0.f, 0.f};
bool s_remoteAimValid = false;
// Событие выстрела от удалённого игрока (колбэк сетевого потока -> Tick).
// Очередь событий выстрела (несколько могут прийти пачкой по TCP).
// Один bool перезаписывал точку следующего выстрела, из-за чего часть
// выстрелов терялась. Сетевой поток добавляет, Tick разбирает по одному.
constexpr int kShotQueueSize = 64;
std::mutex s_shotMtx;
Vec3 s_shotQueue[kShotQueueSize];
int  s_shotQCount = 0;
std::atomic<int> s_shotDropped{0}; // пишется сетевым потоком, читается игровым
std::atomic<bool> s_shotEventsPending{false};
unsigned int s_lastShotEventTaskTick = 0;
unsigned int s_lastShotTick = 0; // минимум паузы между задачами выстрела
// Состояние мира от хоста (время+погода). Игровой поток: забрано из inbox в
// начале Tick().
net::WorldState s_worldState{};
bool s_haveWorldState = false;
std::atomic<bool> s_amHost{false};  // сервер назначил нас хостом
unsigned int s_lastWorldSendTick = 0; // троттлинг отправки WorldState
// P2: Action v2 — действие/сценарий удалённого игрока (из сетевого потока).
std::mutex s_actionMtx;
net::ActionStatePayload s_actionIn{};
bool s_haveActionIn = false;
// Последнее отправленное действие (чтобы не спамить) + heartbeat.
net::ActionStatePayload s_lastSentAction{};
bool s_lastSentActionValid = false;
unsigned int s_lastActionSendTick = 0;
// Что уже применено к педу, чтобы не перезапускать сценарий каждый кадр.
uint8_t  s_appliedActionMode = 0;
uint32_t s_appliedScenario = 0;
uint32_t s_appliedTargetNetId = 0;
uint32_t s_lastLoggedTargetNetId = 0;
unsigned int s_lastCoverTaskTick = 0;
// P2: посадка в транспорт удалённого игрока.
std::mutex s_pvMtx;
net::PlayerVehicleState s_pvIn{};
bool s_havePvIn = false;
net::PlayerVehicleState s_lastSentPv{};
bool s_lastSentPvValid = false;
unsigned int s_lastPvSendTick = 0;
unsigned int s_playerVehicle = 0;
unsigned int s_playerVehicleModel = 0;
bool s_remoteInVehicle = false;
void DeleteRemotePlayerVehicle();
// P3: MissionData-события (хост -> гость).
struct MissionBaseline
{
    bool initialized = false;
    bool completed[missions::kMissionCatalogSize]{};
    int rating[missions::kMissionCatalogSize]{};
};
MissionBaseline s_missionBaseline;
std::mutex s_missionMtx;
std::vector<net::MissionEventPayload> s_missionEvents;
unsigned int s_lastMissionPollTick = 0;
bool s_saveBackupDone = false;
// P3: активная миссия хоста (MissionState).
std::mutex s_missionStateMtx;
net::MissionStatePayload s_missionStateIn{};
bool s_haveMissionState = false;
uint32_t s_remoteMissionId = 0;
uint32_t s_lastSentHostMissionId = 0;
unsigned int s_lastMissionStatePollTick = 0;
unsigned int s_lastMissionStateSendTick = 0;
// P3: барьер совместного старта (offer -> ready -> start window -> started).
struct HostMissionBarrier
{
    uint32_t missionId = 0;
    uint32_t epoch = 0;
    bool ready = false;
    bool started = false;
    bool windowOpen = false;
    unsigned int deadlineTick = 0;
};
struct GuestMissionBarrier
{
    uint32_t missionId = 0;
    uint32_t epoch = 0;
    bool windowOpen = false;
    bool startedSent = false;
    bool eligible = false;
    unsigned int deadlineTick = 0;
};
HostMissionBarrier s_hostBarrier;
GuestMissionBarrier s_guestBarrier;
std::mutex s_barrierMtx;
std::vector<net::MissionBarrierPayload> s_barrierEvents;
uint32_t s_barrierEpoch = 1;
unsigned int s_lastGuestBarrierPollTick = 0;
unsigned int s_lastHostBarrierOfferTick = 0;

// P3: активная Story-миссия хоста по факту запущенного runtime-скрипта.
uint32_t DetectHostActiveMission()
{
    using namespace natives_api;
    Init(natives::GET_MISSION_FLAG);
    if (!Call())
        return 0;

    for (int i = 0; i < missions::kMissionCatalogSize; ++i)
    {
        const missions::MissionInfo& info = missions::kMissionCatalog[i];
        const uint32_t runtimeHash = missions::HashMissionId(info.runtime);
        Init(natives::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH);
        Push(static_cast<unsigned long long>(runtimeHash));
        if (Call() > 0)
            return info.id;
    }
    return 0;
}

// P3: может ли гость реально начать миссию в своём сейве (по MissionData).
bool CheckMissionEligible(uint32_t missionId)
{
    using namespace natives_api;
    Init(natives::MISSIONDATA_IS_VALID);
    Push(static_cast<unsigned long long>(missionId));
    if (!Call()) return false;
    Init(natives::MISSIONDATA_IS_REQUIRED_STORY_MISSION);
    Push(static_cast<unsigned long long>(missionId));
    if (!Call()) return false;
    Init(natives::MISSIONDATA_WAS_COMPLETED);
    Push(static_cast<unsigned long long>(missionId));
    if (Call()) return false;
    return true;
}

void SendMissionBarrier(uint32_t missionId, uint32_t epoch, uint8_t phase,
                        uint8_t flags)
{
    net::MissionBarrierPayload ev{};
    ev.missionId = missionId;
    ev.epoch = epoch;
    ev.phase = phase;
    ev.flags = flags;
    g_netClient.SendMissionBarrier(ev);
}

// Хост: при смене активной миссии — Offer; Ready гостя открывает окно.
void UpdateHostMissionBarrier(uint32_t activeMissionId)
{
    if (activeMissionId != s_hostBarrier.missionId)
    {
        if (activeMissionId)
        {
            s_hostBarrier = HostMissionBarrier{};
            s_hostBarrier.missionId = activeMissionId;
            s_hostBarrier.epoch = ++s_barrierEpoch;
            SendMissionBarrier(activeMissionId, s_hostBarrier.epoch,
                               net::kMissionBarrierOffer, 0);
            s_lastHostBarrierOfferTick = s_tickCount;
            const missions::MissionInfo* info =
                missions::FindMission(activeMissionId);
            sh_log::Write("barrier: offer 0x%08X (%s — %s) epoch=%u\n",
                          activeMissionId, info ? info->code : "????",
                          info ? info->title : "unknown",
                          (unsigned)s_hostBarrier.epoch);
        }
        else if (s_hostBarrier.missionId)
        {
            const uint32_t oldId = s_hostBarrier.missionId;
            const uint32_t oldEpoch = s_hostBarrier.epoch;
            s_hostBarrier = HostMissionBarrier{};
            SendMissionBarrier(oldId, oldEpoch, net::kMissionBarrierAbort, 0);
            sh_log::Write("barrier: abort (host mission ended)\n");
        }
    }

    if (s_hostBarrier.missionId && s_hostBarrier.windowOpen &&
        !s_hostBarrier.started && s_tickCount >= s_hostBarrier.deadlineTick)
    {
        const uint32_t id = s_hostBarrier.missionId;
        const uint32_t ep = s_hostBarrier.epoch;
        s_hostBarrier.windowOpen = false;
        SendMissionBarrier(id, ep, net::kMissionBarrierAbort,
                           net::kMissionBarrierFlagTimeout);
        sh_log::Write("barrier: abort timeout (guest didn't start mission)\n");
    }

    // Гость мог подключиться позже старта миссии — повторяем Offer, пока
    // не получили Ready/не открыли окно (TCP надёжен, но Join мог быть позже).
    if (s_hostBarrier.missionId && !s_hostBarrier.ready &&
        !s_hostBarrier.windowOpen && !s_hostBarrier.started &&
        (s_tickCount - s_lastHostBarrierOfferTick) >= 120)
    {
        SendMissionBarrier(s_hostBarrier.missionId, s_hostBarrier.epoch,
                           net::kMissionBarrierOffer, 0);
        s_lastHostBarrierOfferTick = s_tickCount;
    }
}

// Хост: Ready -> StartWindow, Started -> участие подтверждено.
void ProcessHostMissionBarrier(const net::MissionBarrierPayload& ev)
{
    using namespace coop::missionsbarrier;
    if (!IsHostEventAllowed(s_hostBarrier.missionId, s_hostBarrier.epoch, ev))
        return;
    if (ev.phase == net::kMissionBarrierReady)
    {
        if (!s_hostBarrier.missionId || ev.missionId != s_hostBarrier.missionId ||
            ev.epoch != s_hostBarrier.epoch)
        {
            sh_log::Write("barrier: Ready без активного offer — игнор\n");
            return;
        }
        if (!(ev.flags & net::kMissionBarrierFlagEligible))
        {
            sh_log::Write("barrier: guest NOT eligible — abort\n");
            const uint32_t id = s_hostBarrier.missionId;
            const uint32_t ep = s_hostBarrier.epoch;
            s_hostBarrier = HostMissionBarrier{};
            SendMissionBarrier(id, ep, net::kMissionBarrierAbort, 0);
            return;
        }
        s_hostBarrier.ready = true;
        s_hostBarrier.windowOpen = true;
        s_hostBarrier.deadlineTick = s_tickCount + 45 * 60;
        SendMissionBarrier(s_hostBarrier.missionId, s_hostBarrier.epoch,
                           net::kMissionBarrierStartWindow, 0);
        sh_log::Write("barrier: guest ready -> start window open (45 s)\n");
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      "Guest ready — start the mission!");
        s_subtitleTicksLeft = 8 * 60;
    }
    else if (ev.phase == net::kMissionBarrierStarted)
    {
        if (ev.missionId != s_hostBarrier.missionId ||
            ev.epoch != s_hostBarrier.epoch)
            return;
        s_hostBarrier.started = true;
        s_hostBarrier.windowOpen = false;
        const missions::MissionInfo* info = missions::FindMission(ev.missionId);
        sh_log::Write("barrier: guest STARTED 0x%08X (%s)\n",
                      ev.missionId, info ? info->code : "????");
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      "Guest started: %s",
                      info ? info->title : "mission");
        s_subtitleTicksLeft = 8 * 60;
    }
}

// Гость: Offer -> Ready; StartWindow -> ждём собственный запуск скрипта.
void ProcessGuestMissionBarrier(const net::MissionBarrierPayload& ev)
{
    using namespace coop::missionsbarrier;
    if (!IsGuestEventAllowed(s_guestBarrier.missionId, s_guestBarrier.epoch, ev))
        return;
    if (ev.phase == net::kMissionBarrierOffer)
    {
        s_guestBarrier = GuestMissionBarrier{};
        s_guestBarrier.missionId = ev.missionId;
        s_guestBarrier.epoch = ev.epoch;
        s_guestBarrier.eligible = CheckMissionEligible(ev.missionId);
        const missions::MissionInfo* info = missions::FindMission(ev.missionId);
        SendMissionBarrier(ev.missionId, ev.epoch, net::kMissionBarrierReady,
                           s_guestBarrier.eligible
                               ? net::kMissionBarrierFlagEligible : 0);
        sh_log::Write("barrier: offer 0x%08X accepted=%d (%s — %s)\n",
                      ev.missionId, s_guestBarrier.eligible ? 1 : 0,
                      info ? info->code : "????",
                      info ? info->title : "unknown");
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      s_guestBarrier.eligible
                          ? "Host mission offer: %s"
                          : "Mission unavailable: %s",
                      info ? info->title : "unknown");
        s_subtitleTicksLeft = 8 * 60;
    }
    else if (ev.phase == net::kMissionBarrierStartWindow)
    {
        if (ev.missionId != s_guestBarrier.missionId ||
            ev.epoch != s_guestBarrier.epoch)
            return;
        s_guestBarrier.windowOpen = true;
        s_guestBarrier.startedSent = false;
        s_guestBarrier.deadlineTick = s_tickCount + 45 * 60;
        const missions::MissionInfo* info = missions::FindMission(ev.missionId);
        sh_log::Write("barrier: START WINDOW open for 0x%08X (%s) — 45 s\n",
                      ev.missionId, info ? info->code : "????");
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      "START NOW: %s", info ? info->title : "mission");
        s_subtitleTicksLeft = 8 * 60;
    }
    else if (ev.phase == net::kMissionBarrierAbort)
    {
        sh_log::Write("barrier: abort (flags=0x%02X)\n", (unsigned)ev.flags);
        s_guestBarrier = GuestMissionBarrier{};
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      "Mission start window closed");
        s_subtitleTicksLeft = 6 * 60;
    }
}

// P3: бэкап сейва гостя перед первым зачётом миссии. Копирует известные
// каталоги профилей (repack .1911 и официальный Rockstar) в Documents.
// Возвращает false, если сейвы не найдены/копирование не удалось — тогда
// зачёт НЕ пишем (защита от порчи прогресса).
bool EnsureGuestSaveBackup()
{
    if (!coopcfg::Get().saveBackup)
        return true;
    if (s_saveBackupDone)
        return true;

    namespace fs = std::filesystem;
    std::error_code ec;

    const char* appData = std::getenv("APPDATA");
    const char* userProfile = std::getenv("USERPROFILE");
    const char* oneDrive = std::getenv("OneDrive");

    std::vector<fs::path> roots;
    if (appData)
    {
        roots.emplace_back(fs::path(appData) / ".1911" /
                           "Red Dead Redemption 2" / "profile");
    }
    if (userProfile)
    {
        roots.emplace_back(fs::path(userProfile) / "Documents" /
                           "Rockstar Games" / "Red Dead Redemption 2" / "Profiles");
    }
    if (oneDrive)
    {
        roots.emplace_back(fs::path(oneDrive) / "Documents" /
                           "Rockstar Games" / "Red Dead Redemption 2" / "Profiles");
    }

    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32] = {0};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    fs::path base;
    if (userProfile)
        base = fs::path(userProfile) / "Documents" / "rdr2-coop-saves";
    else
        base = fs::current_path() / "rdr2-coop-saves";
    fs::path dest = base / stamp;
    fs::create_directories(dest, ec);

    int copiedFiles = 0;
    int copiedRoots = 0;
    std::string manifest;
    for (const fs::path& root : roots)
    {
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec))
            continue;
        const fs::path target = dest / root.filename();
        // root.filename() для .1911/profile = "profile"; добавьте уникальность
        // по родителю, чтобы не перетереть при совпадении имён.
        fs::path uniqueTarget = target;
        int suffix = 1;
        while (fs::exists(uniqueTarget, ec))
            uniqueTarget = target.string() + "-" + std::to_string(suffix++);
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (!it->is_regular_file(ec))
                continue;
            const fs::path relative = fs::relative(it->path(), root, ec);
            if (ec)
                continue;
            const fs::path out = uniqueTarget / relative;
            fs::create_directories(out.parent_path(), ec);
            fs::copy_file(it->path(), out,
                          fs::copy_options::overwrite_existing, ec);
            if (!ec)
                ++copiedFiles;
            ec.clear();
        }
        ++copiedRoots;
        {
            fs::path rel = fs::relative(uniqueTarget, dest, ec);
            if (ec || rel.empty())
                rel = uniqueTarget.filename();
            ec.clear();
            manifest += "backup=" + rel.generic_string() +
                        "|root=" + root.generic_string() + "\n";
        }
    }

    if (copiedFiles == 0)
    {
        sh_log::Write("mission: save backup FAILED — saves not found "
                      "(mission_credit aborted; set save_backup=0 to override)\n");
        return false;
    }

    // Манифест нужен scripts\restore_saves.ps1: куда какой каталог бэкапа
    // возвращать. Абсолютные пути намеренно не хардкодятся.
    {
        std::ofstream mf(dest / "manifest.txt", std::ios::trunc);
        if (mf)
            mf << manifest;
    }

    s_saveBackupDone = true;
    sh_log::Write("mission: save backup OK -> %s (roots=%d files=%d)\n",
                  dest.string().c_str(), copiedRoots, copiedFiles);
    return true;
}
// Последнее применённое время (для лога смены).
int s_lastAppliedHour = -1, s_lastAppliedMin = -1;
// Компоненты одежды удалённого игрока. Игровой поток: забрано из inbox в
// начале Tick(); сетевой поток сюда больше не пишет.
net::OutfitState s_outfitIn{};
bool s_haveOutfit = false;
// Последний отправленный набор компонентов (чтобы слать при смене).
net::OutfitState s_sentOutfit{};
bool s_sentOutfitValid = false;
// Дебаунс смены наряда: набор должен продержаться ~0.5 с, иначе оружие,
// кобуры и патронташи при доставании дёргают список компонентов и наряд
// переприменяется без необходимости. Плюс диагностика изменённых позиций.
net::OutfitState s_candidateOutfit{};
bool s_haveCandidateOutfit = false;
unsigned int s_candidateOutfitTicks = 0;
unsigned int s_lastOutfitSendTick = 0;
constexpr unsigned int kOutfitDebounceTicks = 30; // ~0.5 с при 60 Гц
constexpr unsigned int kOutfitMinIntervalTicks = 30;
// Heartbeat: даже без смены наряда переотправляем его раз в ~5 с, иначе после
// реконнекта/пересоздания педа у получателя остаётся случайная одежда
// (отправитель больше не шлёт, т.к. s_sentOutfitValid уже true).
constexpr unsigned int kOutfitHeartbeatTicks = 5 * 60;
unsigned int s_lastHorseOutfitSendTick = 0;
// Игрок ожил после смерти (сервер прислал Respawned) — возродить педа.
// Заполняется игровым потоком из inbox; имя использует RespawnGhostPed.
char s_remoteRespawnName[64] = "";
// Сколько тиков ждём первый кадр (модель персонажа) перед спавном педа.
unsigned int s_pendingJoinWaitTicks = 0;
// Отложенный наряд: применён, только когда модель педа совпадёт с моделью
// наряда (пед пересоздаётся по модели персонажа из кадра reserved[2]).
net::OutfitState s_pendingOutfit{};
bool s_havePendingOutfit = false;
// Последний ФАКТИЧЕСКИ надетый на педа наряд. Нужен, чтобы не применять один
// и тот же набор повторно: каждое APPLY_SHOP_ITEM_TO_PED добавляет слой
// одежды, и через несколько применений старая одежда начинает просвечивать
// сквозь новую (наблюдали у педа после возрождения).
net::OutfitState s_appliedOutfit{};
bool s_appliedOutfitValid = false;
// Базовая вариация педа рандомизируется ОДИН раз на создание/респавн педа.
// Повторная рандомизация в ApplyOutfitToGhost/RefreshGhostComponents меняла
// базовые компоненты (и подмешивала случайные вещи в категориях, где у
// оригинала пусто) — наряд расходился с оригиналом.
bool s_ghostBaseRandomized = false;
// Тиков, в течение которых периодически переприменяем наряд (после
// посадки/спешивания движок сам сбрасывает variation и внешний вид «слетает»).
unsigned int s_outfitRefreshTicks = 0;
// Флаг «соединение потеряно, идёт reconnect» (inbox -> Tick, игровой поток).
bool s_pendingConnectionLost = false;
// Флаг «игрок вышел»: обработка (удаление педа) произойдёт в Tick().
bool s_pendingLeave = false;
char s_leaveName[64] = ""; // имя ушедшего игрока (для надписи «left»)
// Последнее применённое к педу состояние движения (сброс при выходе).
uint8_t s_lastMoveState = 0xFF;
// «Догоняем» ли стоящего игрока: гистерезис для Idle. Без него призрак,
// стоящий в 0.5–1 м, из-за покачивания корпуса постоянно переходит
// Idle↔Walk и «пританцовывает» (шаффлит ногами).
bool s_idleChase = false;
// Дебаунс определения move: переключаем только после нескольких подряд
// одинаковых замеров. Иначе стоящий игрок мерцает Walk/Idle (IS_PED_WALKING
// дрожит + скорость на месте колеблется) → пед-призрак дёргается (задачи
// движения/стояния переключаются каждый кадр).
uint8_t s_moveStable = 0xFF;      // текущее стабильное состояние
uint8_t s_moveCandidate = 0xFF;   // кандидат на смену
int     s_moveCandidateCount = 0; // сколько подряд замеров совпало
constexpr int kMoveDebounce = 24; // ~0.4 с при отправке КАЖДЫЙ тик (60 Гц)
// Тик последнего прижима педа к земле в покое (троттлинг, не чаще раза в сек).
unsigned int s_lastIdleGroundFixTick = 0;
// RTT-окно для экранного индикатора (мин/макс за последние ~2 с).
uint32_t s_rttMin = 0, s_rttMax = 0;
unsigned int s_lastRttDrawTick = 0;
uint32_t s_rttPrev = 0;      // предыдущий замер (для джиттера)
float    s_rttJitter = 0.f;  // сглаженный джиттер, мс
char     s_rttHud[96] = "";  // последняя сформированная строка HUD
char     s_rttLine[96] = "RTT -"; // строка HUD (рисуется каждый кадр)
// «Грация» после возрождения педа: несколько тиков не считаем его мёртвым
// (RESURRECT_PED применяется не мгновенно — защита от ложного повторного
// срабатывания блока локальной смерти).
unsigned int s_reviveGraceTicks = 0;
// Последнее применённое особое состояние/оружие (сброс при выходе).
uint8_t s_lastPedState = 0xFF;
unsigned long long s_lastWeapon = 0;
// Оружие, реально выданное педу-призраку (чтобы снять при смене на «без оружия»).
unsigned long long s_givenWeapon = 0;
// Латч «оружие уже взято в руку». Без него EnsureGhostWeaponInHands дёргал
// SET_CURRENT_PED_WEAPON(equipNow=1) 10-12 раз/с (на каждый aim/shoot) —
// это переигрывало анимацию доставания и сбивало TASK_SHOOT_AT_COORD.
// Сбрасывается в ResetGhostCombatState (смерть/респавн/cleanup/model change),
// при смене/снятии оружия, выходе из боя и на переходах лошади.
bool s_weaponEquippedInHands = false;
unsigned long long s_weaponEquippedHash = 0;
// 0 = IS_WEAPON_VALID ещё не проверяли, 1 = рабочий, -1 = недоступен/неверен
// (тогда валидацию оружия не применяем, чтобы не потерять синхронизацию).
int s_weaponValidProbe = 0;

// Сбросить латч «оружие в руках»: оружие могло уйти в кобуру или смениться
// (выход из боя, посадка/спешивание, смена/снятие оружия, респавн).
void InvalidateGhostWeaponLatch()
{
    s_weaponEquippedInHands = false;
    s_weaponEquippedHash = 0;
}
// Боевое действие активно (прицеливание/стрельба/перезарядка): пед стоит и
// держит оружие, задачи движения не ставим (они отменяют боевую задачу).
bool s_combatActive = false;
// Тик последней выдачи задачи стрельбы (перезадаём, пока стрельба идёт).
unsigned int s_lastShootTaskTick = 0;
// «Залипание» выстрела на получателе: пока >0, прицел НЕ перебивает задачу
// стрельбы (иначе кадр aim, пришедший сразу за shoot, отменял выстрел до
// срабатывания).
unsigned int s_shootHoldTicks = 0;
// Выход из боя отложен, пока доигрывает выстрел.
bool s_pendingCombatExit = false;
// Куда пед наведён сейчас (чтобы перенаводить при движении цели: прицел
// с time=-1 держится за ФИКСИРОВАННОЙ точкой и сам не обновляется).
Vec3 s_lastAimTaskPoint{0.f, 0.f, 0.f};
bool s_lastAimTaskValid = false;
unsigned int s_lastAimTaskTick = 0;
// Последнее применённое действие (для применения при смене).
uint8_t s_lastAction = 0xFF;

// Полный сброс боевого состояния призрака (смерть/респавн/cleanup).
// Оружие, hold и очередь выстрелов не должны переживать пересоздание педа:
// иначе после смерти/респавна выстрелы молча не работают (s_lastWeapon
// считался «уже выданным», а задача стрельбы висела на старом трупе).
void ResetGhostCombatState()
{
    s_lastWeapon = 0;
    s_givenWeapon = 0;
    InvalidateGhostWeaponLatch();
    s_combatActive = false;
    s_shootHoldTicks = 0;
    s_lastShootTaskTick = 0;
    s_lastShotEventTaskTick = 0;
    s_pendingCombatExit = false;
    s_lastAimTaskValid = false;
    s_lastAction = 0xFF;
    s_lastPedState = 0xFF;
    {
        std::lock_guard<std::mutex> lk(s_shotMtx);
        s_shotQCount = 0;
    }
    s_shotEventsPending.store(false);
    s_shotDropped.store(0);
}

// Лошадь для верховой езды удалённого игрока (0 = нет).
unsigned int s_mount = 0;
bool s_mountModelRequested = false;
// Модель лошади, которую надо создать (из кадра; 0 = стандартная).
unsigned long long s_mountWantedModel = 0;
unsigned long long s_ignoredMountModelHash = 0; // невалидный хэш (лог один раз)
// Позиция лошади (последняя цель из кадра), чтобы двигать её за педом.
Vec3 s_mountTarget{0.f, 0.f, 0.f};
bool s_haveMountTarget = false;
// Последняя цель, на которую лошади выдана задача движения (чтобы не
// перезадавать её каждый кадр — иначе лошадь дёргается).
Vec3 s_mountTaskTarget{0.f, 0.f, 0.f};
bool s_mountTaskValid = false;
// Сколько тиков цель кадра не двигалась — для принудительного «сеттла»
// лошади, если она отстала, пока сервер/сеть догоняли позицию.
unsigned int s_mountStillTicks = 0;
// Предыдущая цель кадра для лошади. Нужна, чтобы отличать НАСТОЯЩИЙ
// телепорт игрока (респавн/смена локации) от отставания на галопе.
Vec3 s_prevMountTarget{0.f, 0.f, 0.f};
bool s_prevMountTargetValid = false;
// Тик последней попытки пересадить всадника, если он «слетел» с лошади.
unsigned int s_lastMountReseatTick = 0;
// «Грация» верховой езды заменена фазовой машиной: кадры move=RideHorse
// могут кратко мерцать на Walk/Idle (IS_PED_ON_MOUNT дрожит), поэтому
// спешивание начинаем только после kRideLossTicks подряд «не верхом».
unsigned int s_rideLossTicks = 0;
constexpr unsigned int kRideLossTicks = 15; // ~0.25 с
// Фаза взаимодействия педа с лошадью (для анимаций посадки/спешивания).
enum class MountPhase : uint8_t
{
    None = 0,    // лошади нет
    Mounting,    // пед идёт к лошади и садится (анимация)
    Seated,      // в седле — ведём лошадь за кадрами
    Dismounting, // пед слезает (анимация), лошадь ещё стоит
};
MountPhase s_mountPhase = MountPhase::None;
unsigned int s_mountPhaseTicks = 0;
// Телепорт лошади — ТОЛЬКО при реальной потере (респавн/смена локации),
// а не при отставании на галопе. Раньше порог был 10 м, и при быстрой
// скачке задача не успевала — каждый раз срабатывал телепорт.
constexpr float TELEPORT_DIST_MOUNT = 60.0f;
// Скачок цели между соседними кадрами: настоящий телепорт игрока
// (респавн), а не нормальное движение (на галопе ~0.3–0.5 м за кадр).
constexpr float TELEPORT_TARGET_JUMP = 15.0f;

// Мета-наряд удалённого игрока: хотим применить к педу призрака.
// Запрос асинхронный (REQUEST -> ждём -> APPLY), поэтому храним состояние.
unsigned long long s_wantedOutfit = 0;       // последний запрошенный хэш наряда
int    s_outfitRequest = 0;                  // id запроса REQUEST_META_PED_OUTFIT
bool   s_outfitRequestPending = false;       // запрос в полёте
bool   s_outfitRequestLogged = false;        // логировали ли ожидание

// Снаряжение лошади удалённого игрока (седло/поклажа): мета-наряд лошади
// всегда 0, поэтому, как и одежду, синхронизируем shop-предметы через
// GET_SHOP_ITEM_COMPONENT_AT_INDEX + APPLY_SHOP_ITEM_TO_PED.
// s_horseOutfitIn — игровой поток: забрано из inbox в начале Tick().
net::OutfitState s_horseOutfitIn{};
bool s_haveHorseOutfit = false;
net::OutfitState s_horseOutfitApplied{};// фактически надето на лошадь
bool s_horseOutfitAppliedValid = false;
net::OutfitState s_sentHorseOutfit{};   // последний отправленный набор (отправитель)
bool s_sentHorseOutfitValid = false;

// Посадить педа призрака на лошадь (объявлено здесь — используется в
// RequestMountModel ниже; определение дальше).
void PromoteMountToGhost();
// Применить снаряжение лошади (определение дальше).
void ApplyHorseOutfitToMount(const net::OutfitState& os);
// Пересобрать внешний вид педа (component variation + наряд). Определение ниже.
void RefreshGhostComponents();
// Анимации посадки/спешивания (определения ниже).
void StartMounting();
void StartDismounting();
bool IsGhostMounted();

// Запросить и загрузить модель лошади (вызывается в Tick, игровой поток).
void RequestMountModel()
{
    using namespace natives_api;
    if (s_mount && DoesEntityExist(s_mount)) return; // лошадь уже есть
    unsigned long long model = s_mountWantedModel ? s_mountWantedModel
                                                  : natives::HORSE_MODEL;
    if (!IsModelHashValid(model))
    {
        // Хэш из сети оказался невалидным — откатываемся на стандартную
        // модель лошади (невалидный REQUEST_MODEL/CREATE_PED не даст лошадь).
        if (s_mountWantedModel)
            sh_log::Write("coop: invalid mount model 0x%llX, using default\n",
                          s_mountWantedModel);
        s_mountWantedModel = 0;
        model = natives::HORSE_MODEL;
        if (!IsModelHashValid(model))
            return; // нет валидной модели — не залипаем, просто без лошади
    }
    // Запрашиваем модель КАЖДЫЙ раз, пока она не загрузилась (движок
    // идемпотентен; не полагаемся на однократный флаг — если модель не
    // успела загрузиться к первому кадру, без ретрая лошадь не появится).
    Init(natives::REQUEST_MODEL);
    Push(model);
    Push(static_cast<unsigned long long>(0)); // p1: 2-й аргумент обязателен
    Call();
    s_mountModelRequested = true;
    if (s_tickCount % (1 * 60) == 1) // лог не чаще раза в секунду
        sh_log::Write("coop: request horse model 0x%llX\n", model);
    if (!HasModelLoaded(model))
    {
        if (s_tickCount % (1 * 60) == 1)
            sh_log::Write("coop: waiting horse model 0x%llX...\n", model);
        return;
    }
    // Ставим лошадь РЯДОМ С ПЕДОМ (справа ~1.6 м): по сценарию пед сначала
    // подходит и садится с анимацией (TASK_MOUNT_ANIMAL), а не телепортируется
    // в седло. Раньше лошадь создавалась прямо в позиции всадника.
    Vec3 at = GetEntityCoords(s_ghost);
    {
        float h = GetEntityHeading(s_ghost) * 0.01745329251f; // рад.
        at.x += 1.6f * std::cos(h);
        at.y += 1.6f * std::sin(h);
    }
    {
        float gz = 0.f;
        if (GetGroundZForCoord(at, gz))
            at.z = gz + 0.2f;
        else
            at.z -= 1.2f; // землю не нашли — хотя бы не в воздухе
    }
    Init(natives::CREATE_PED);
    Push(model);
    Push(at.x); Push(at.y); Push(at.z);
    Push(0.f);
    Push(false); Push(false); Push(false); Push(false);
    s_mount = static_cast<unsigned int>(Call());
    if (s_mount)
    {
        // Обвязка видимости, как у человеческого педа: mission+variation+visible.
        Init(natives::SET_ENTITY_AS_MISSION_ENTITY); Push(s_mount); Push(true); Push(true); Call();
        // variation НУЖНА для рендера лошади (как и педа) — без неё лошадь
        // невидима. Масть задаётся моделью из кадра (0x28F9976A и т.п.),
        // variation рандомизирует только текстуру-вариацию, сохраняя модель.
        Init(natives::SET_PED_RANDOM_COMPONENT_VARIATION); Push(s_mount); Push(0ull); Call();
        Init(natives::SET_ENTITY_VISIBLE); Push(s_mount); Push(true); Call();
        Init(natives::PLACE_OBJECT_ON_GROUND_PROPERLY); Push(s_mount); Call();
        // Коллизию лошади НЕ отключаем: с выключенной коллизией она
        // проваливается сквозь землю и «падает» бесконечно. Столкновения
        // снимаем точечно — только с локальным игроком и его лошадью
        // (SET_ENTITY_NO_COLLISION_ENTITY в ApplyRemoteFrame).
        s_mountTaskValid = false;
        sh_log::Write("coop: horse created handle=%u at (%.1f %.1f %.1f)\n",
                      s_mount, at.x, at.y, at.z);
        // НЕ сажаем сразу: ApplyRemoteFrame запустит TASK_MOUNT_ANIMAL,
        // чтобы пед подошёл и сел с анимацией.
        s_mountPhase = MountPhase::None;
        s_mountPhaseTicks = 0;
    }
    s_mountModelRequested = false; // при неудаче позволим повторить запрос
}

// Посадить педа призрака на лошадь (если лошадь создана и пед жив).
void PromoteMountToGhost()
{
    if (!s_ghost || !DoesEntityExist(s_ghost) || !s_mount || !DoesEntityExist(s_mount)) return;
    // На лошади движок меняет способ отображения оружия — после посадки
    // переэкипируем один раз (латч сбрасываем).
    InvalidateGhostWeaponLatch();
    using namespace natives_api;
    Init(natives::SET_PED_ONTO_MOUNT);
    Push(s_ghost);
    Push(s_mount);
    Push(-1);  // seatIndex
    Push(false);
    Call();
    sh_log::Write("coop: mounted ped on horse\n");
    // После посадки движок сбрасывает component variation педа — модель
    // «исчезает», остаются только надетыми одёжки. Возвращаем вариацию и наряд.
    RefreshGhostComponents();
    // Снаряжение лошади применяется после посадки (пришло раньше по сети).
    if (s_horseOutfitIn.count)
    {
        s_horseOutfitAppliedValid = false;
        ApplyHorseOutfitToMount(s_horseOutfitIn);
    }
}

// Пед подходит к лошади и садится С АНИМАЦИЕЙ (TASK_MOUNT_ANIMAL).
// timer = 4 с: если пед не дойдёт (кривой навмеш), движок сам посадит его.
void StartMounting()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    if (!s_mount || !DoesEntityExist(s_mount)) return;
    // Во время посадки/верхом призрак может убрать оружие — латч сбрасываем.
    InvalidateGhostWeaponLatch();
    using namespace natives_api;
    Init(natives::TASK_MOUNT_ANIMAL);
    Push(s_ghost);
    Push(s_mount);
    Push(static_cast<unsigned long long>(4000)); // timer (ms): авто-посадка
    // seatIndex = -1 (как в рабочем SET_PED_ONTO_MOUNT): 0 — это ПАССАЖИРСКОЕ
    // место на крупе, из-за него пед садился «за спиной» всадника.
    Push(static_cast<unsigned long long>(-1));   // seatIndex
    Push(1.0f);                                  // pedSpeed
    Push(static_cast<unsigned long long>(0));    // mountStyle
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
    s_mountPhase = MountPhase::Mounting;
    s_mountPhaseTicks = 0;
    sh_log::Write("coop: mount animation started\n");
}

// Пед слезает С АНИМАЦИЕЙ (TASK_DISMOUNT_ANIMAL). Лошадь удалим после.
void StartDismounting()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    // Спешивание тоже может менять/убирать оружие — латч сбрасываем.
    InvalidateGhostWeaponLatch();
    using namespace natives_api;
    Init(natives::TASK_DISMOUNT_ANIMAL);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(0)); // taskFlag (сторона спешивания)
    Push(static_cast<unsigned long long>(0)); // p2
    Push(static_cast<unsigned long long>(0)); // p3
    Push(static_cast<unsigned long long>(0)); // p4
    Push(static_cast<unsigned long long>(0)); // targetPed
    Call();
    s_mountPhase = MountPhase::Dismounting;
    s_mountPhaseTicks = 0;
    // Окно переприменения наряда: во время спешивания движок сбрасывает
    // component variation, из-за чего «слетает» модель/одежда.
    s_outfitRefreshTicks = 240; // ~4 с
    sh_log::Write("coop: dismount animation started\n");
}

bool IsGhostMounted()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return false;
    using namespace natives_api;
    Init(natives::IS_PED_ON_MOUNT);
    Push(s_ghost);
    return Call() != 0;
}

// Двигать лошадь к цели кадра. ВАЖНО: двигаем ЗАДАЧЕЙ по навмешу, а не
// телепортом. SetEntityCoords на лошади с седоком ломает физику: всадник
// слетает, лошадь «взлетает» и падает. Задача ведёт лошадь по земле, седок
// остаётся в седле, анимации играются движком.
void MoveMountTo(const Vec3& target)
{
    if (!s_mount || !DoesEntityExist(s_mount)) return;
    using namespace natives_api;
    Vec3 cur = GetEntityCoords(s_mount);
    float dx = target.x - cur.x, dy = target.y - cur.y;
    float dist = std::sqrt(dx * dx + dy * dy);

    // Отличаем «настоящий» телепорт цели (респавн/смена локации) от
    // отставания на галопе. На галопе цель смещается на ~0.3–0.5 м/кадр,
    // поэтому скачок > 15 м между кадрами — это точно телепорт.
    float targetJump = 0.f;
    if (s_prevMountTargetValid)
    {
        const float tx = target.x - s_prevMountTarget.x;
        const float ty = target.y - s_prevMountTarget.y;
        targetJump = std::sqrt(tx * tx + ty * ty);
    }
    s_prevMountTarget = target;
    s_prevMountTargetValid = true;

    // Считаем, сколько тиков цель стоит на месте: если лошадь отстала, пока
    // сервер/сеть догоняли позицию, после остановки цели её нужно подтянуть.
    if (targetJump < 0.05f)
        ++s_mountStillTicks;
    else
        s_mountStillTicks = 0;

    const bool realTeleport = (targetJump > TELEPORT_TARGET_JUMP) ||
                              (dist > TELEPORT_DIST_MOUNT);
    if (realTeleport)
    {
        // Респавн/телепорт: переносим лошадь вместе с седоком и сажаем заново.
        Vec3 at = target;
        float gz = 0.f;
        if (GetGroundZForCoord(at, gz))
            at.z = gz + 0.2f;
        SetEntityCoords(s_mount, at);
        // Если всадник уже в седле, он переедет вместе с лошадью; ручной
        // перенос педа в этом случае только ломает посадку.
        if (!IsGhostMounted())
            SetEntityCoords(s_ghost, at);
        PromoteMountToGhost();
        s_mountTaskValid = false;
        s_mountStillTicks = 0;
        sh_log::Write("coop: mount teleport (dist=%.1f jump=%.1f)\n", dist, targetJump);
        return;
    }

    // Страховка: всадник «слетел» (провал задачи посадки) — пересаживаем,
    // иначе лошадь уедет, а пед останется и будет дёргаться/телепортироваться.
    if (s_mountPhase == MountPhase::Seated && !IsGhostMounted() &&
        (s_tickCount - s_lastMountReseatTick) > 60)
    {
        s_lastMountReseatTick = s_tickCount;
        PromoteMountToGhost();
        s_mountTaskValid = false;
        return;
    }

    // Сеттл после лага: цель уже стоит, а лошадь всё ещё не рядом.
    // Иначе при пропущенных кадрах она остаётся «не на том месте».
    if (dist > 2.0f && s_mountStillTicks >= 90)
    {
        Vec3 at = target;
        float gz = 0.f;
        if (GetGroundZForCoord(at, gz))
            at.z = gz + 0.2f;
        SetEntityCoords(s_mount, at);
        if (!IsGhostMounted())
            SetEntityCoords(s_ghost, at);
        PromoteMountToGhost();
        s_mountTaskValid = false;
        s_mountStillTicks = 0;
        sh_log::Write("coop: mount settle (dist=%.1f, target stationary)\n", dist);
        return;
    }

    if (dist > 5.0f && (s_tickCount % 120) == 0)
        sh_log::Write("coop: mount lag dist=%.1f jump=%.2f still=%u\n",
                      dist, targetJump, s_mountStillTicks);

    if (dist < 0.5f)
    {
        s_mountStillTicks = 0;
        return; // почти на месте — задачу не трогаем
    }
    // Перезадаём задачу только когда цель заметно сдвинулась (0.5 м):
    // частая перезадача дёргает лошадь.
    if (s_mountTaskValid &&
        std::fabs(target.x - s_mountTaskTarget.x) < 0.5f &&
        std::fabs(target.y - s_mountTaskTarget.y) < 0.5f)
        return;

    // Догоняем ЗАДАЧЕЙ, а не телепортом: чем больше отставание, тем выше
    // множитель скорости (3.0 — галоп). Раньше всегда было 2.0, лошадь не
    // успевала за галопом, разрыв рос и срабатывал телепорт.
    float speed = 1.0f;
    if (dist > 12.0f)      speed = 3.0f;
    else if (dist > 3.0f)  speed = 2.0f;
    if (targetJump > 0.5f) speed = std::max(speed, 2.0f); // цель быстро уходит

    Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
    Push(s_mount);
    Push(target.x); Push(target.y); Push(target.z);
    Push(speed);
    Push(static_cast<unsigned long long>(0xFFFFFFFF));
    Push(1.0f);
    Push(static_cast<unsigned long long>(4));
    Push(40000.f);
    Call();
    s_mountTaskTarget = target;
    s_mountTaskValid = true;
}

// Отменить верховую езду: убрать лошадь. Педа НЕ трогаем позицией (его
// поставит следующий кадр), но пересобираем внешний вид — после удаления
// лошади из-под седока движок может сбросить variation (остаются одёжки).
void DismountAndRemoveMount()
{
    // Лошадь удаляется из-под седока — оружие/латч точно неактуальны.
    InvalidateGhostWeaponLatch();
    if (s_mount && DoesEntityExist(s_mount))
    {
        using namespace natives_api;
        int m = static_cast<int>(s_mount);
        Init(natives::DELETE_PED);
        Push(reinterpret_cast<unsigned long long>(&m));
        Call();
        sh_log::Write("coop: horse removed\n");
    }
    s_mount = 0;
    s_mountModelRequested = false;
    s_haveMountTarget = false;
    s_mountWantedModel = 0;
    s_mountTaskValid = false;
    s_mountPhase = MountPhase::None;
    s_mountPhaseTicks = 0;
    RefreshGhostComponents();
    // Сброс состояния снаряжения лошади (лошади больше нет).
    s_horseOutfitApplied = net::OutfitState{};
    s_horseOutfitAppliedValid = false;
}

// Снять столкновения лошади-призрака с локальным игроком и его лошадью.
// Мир/земля при этом остаются коллизионными (иначе лошадь проваливается).
// thisFrameOnly=true → вызывать каждый кадр.
void DisableMountLocalCollision()
{
    if (!s_mount || !DoesEntityExist(s_mount)) return;
    using namespace natives_api;
    unsigned int playerPed = GetPlayerPedFor(0);
    if (!playerPed) return;
    Init(natives::SET_ENTITY_NO_COLLISION_ENTITY);
    Push(s_mount); Push(playerPed); Push(static_cast<unsigned long long>(1));
    Call();
    Init(natives::GET_MOUNT); Push(playerPed);
    unsigned int pm = static_cast<unsigned int>(Call());
    if (pm)
    {
        Init(natives::SET_ENTITY_NO_COLLISION_ENTITY);
        Push(s_mount); Push(pm); Push(static_cast<unsigned long long>(1));
        Call();
    }
}
// Педу-призраку не даём уставать: иначе после бега за игроком он «отдувается»
// (анимация тяжёлого дыхания/усталости), даже стоя. Множитель истощения
// выносливости = 0 (не устаёт) + полное восстановление.
void SetGhostStaminaFull()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    Init(natives::SET_STAMINA_DEPLETION_MULTIPLIER);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(0));
    Call();
    Init(natives::GET_PED_MAX_STAMINA);
    Push(s_ghost);
    float maxStam = CallFloat();
    if (maxStam <= 0.f) maxStam = 1.0f;
    Init(natives::RESTORE_PED_STAMINA);
    Push(s_ghost);
    Push(maxStam);
    Call();
}

// Последняя цель задачи движения (чтобы не перезадавать задачу каждый кадр).
Vec3 s_moveTarget{0.f, 0.f, 0.f};
bool s_moveTargetValid = false;

// Применить move-состояние к педу призрака (объявлено здесь — используется
// в ApplyRemoteFrame ниже; определение дальше в файле).
void ApplyMoveToGhost(unsigned int ped, uint8_t move, const Vec3& target);

// Заморозить/разморозить физику педа (идл-пед стоит как статуя и его не
// толкают коллизии игрока; движение — только ручным SetEntityCoords).
void SetGhostFrozen(bool freeze)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    Init(natives::FREEZE_ENTITY_POSITION);
    Push(s_ghost);
    Push(freeze ? 1 : 0);
    Call();
}

// Точка прицеливания педа: 15 м вперёд по его heading на уровне груди.
// Направление в RDR2: heading 0 = +Y, вперёд = (-sin h, cos h).
Vec3 GhostAimPointAhead()
{
    // 1) Точка прицела от удалённого игрока (если она не рядом с педом).
    if (s_remoteAimValid)
    {
        Vec3 p = GetEntityCoords(s_ghost);
        float dx = s_remoteAim.x - p.x, dy = s_remoteAim.y - p.y, dz = s_remoteAim.z - p.z;
        if (dx * dx + dy * dy + dz * dz > 2.25f) // >1.5 м
            return s_remoteAim;
    }
    // 2) Иначе — вперёд по heading.
    Vec3 p = GetEntityCoords(s_ghost);
    float h = GetEntityHeading(s_ghost) * 0.01745329251f;
    Vec3 t;
    t.x = p.x - std::sin(h) * 15.0f;
    t.y = p.y + std::cos(h) * 15.0f;
    t.z = p.z + 1.2f;
    return t;
}

// IS_WEAPON_VALID (0x937C71165CF334B3) — хэш сверен с rdr2-runtime NativeDecl;
// в src/natives.h его пока нет (зона Lead), поэтому локальная константа.
constexpr unsigned long long kIsWeaponValid = 0x937C71165CF334B3ull;

// Валидация weaponHash, пришедшего из сети. Чтобы не потерять синхронизацию
// оружия, если хэш натива окажется неверным/недоступным, один раз проверяем
// его на оружии, которое заведомо в руках у ЛОКАЛЬНОГО игрока: если натив не
// подтверждает даже его — валидацию отключаем (пропускаем любой ненулевой хэш).
bool IsWeaponHashValid(unsigned long long weapon)
{
    if (!weapon || weapon == 0xA2719263ull) // 0 / Unarmed
        return false;
    using namespace natives_api;
    if (s_weaponValidProbe == 0)
    {
        unsigned int localPed = GetPlayerPedFor(0);
        unsigned long long current = 0;
        if (localPed)
        {
            Init(natives::GET_CURRENT_PED_WEAPON);
            Push(localPed);
            Push(reinterpret_cast<unsigned long long>(&current));
            Push(static_cast<unsigned long long>(1));
            Push(static_cast<unsigned long long>(0));
            Push(static_cast<unsigned long long>(0));
            Call();
        }
        if (current && current != 0xA2719263ull)
        {
            Init(kIsWeaponValid);
            Push(current);
            s_weaponValidProbe = (Call() != 0) ? 1 : -1;
            sh_log::Write("coop: IS_WEAPON_VALID probe (%s)\n",
                          s_weaponValidProbe > 0 ? "ok" : "unavailable; validation off");
        }
    }
    // Натив не подтверждён (нет оружия у локального игрока) или недоступен —
    // не фильтруем, чтобы не потерять синхронизацию оружия из-за неизвестного
    // хэша натива (поведение как до валидации).
    if (s_weaponValidProbe <= 0)
        return true;
    Init(kIsWeaponValid);
    Push(weapon);
    return Call() != 0;
}

// Перед боевой задачей убедиться, что у призрака оружие В РУКАХ: без этого
// TASK_SHOOT_AT_COORD/TASK_AIM_GUN_AT_COORD молча не работают (натив ничего
// не делает, если оружие в кобуре). Это фикс 2d23c4b, потерянный при откате:
// призрак убирает оружие после боя/посадки/респавна, и выстрелы «пропадают».
void EnsureGhostWeaponInHands()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    unsigned long long weapon = s_givenWeapon ? s_givenWeapon : s_lastWeapon;
    if (!weapon) return;
    // Битый weaponHash из сети не должен попадать в GIVE/SET_CURRENT.
    if (!IsWeaponHashValid(weapon))
        return;
    using namespace natives_api;
    if (!s_givenWeapon)
    {
        // Оружие ещё не выдавалось (новый пед после смерти/пересоздания) —
        // выдаём заново, иначе задача стрельбы уйдёт в пустоту.
        Init(natives::GIVE_WEAPON_TO_PED);
        Push(s_ghost);
        Push(weapon);
        Push(static_cast<unsigned long long>(60)); // ammoCount
        Push(static_cast<unsigned long long>(0));  // bForceInHand
        Push(static_cast<unsigned long long>(0));  // bForceInHolster
        Push(static_cast<unsigned long long>(0));  // attachPoint
        Push(static_cast<unsigned long long>(0));  // bAllowMultipleCopies
        Push(0.5f);                                // p7
        Push(1.0f);                                // p8
        Push(static_cast<unsigned long long>(0));  // addReason
        Push(static_cast<unsigned long long>(1));  // bIgnoreUnlocks
        Push(0.5f);                                // permanentDegradation
        Push(static_cast<unsigned long long>(0));  // p12
        Call();
        s_givenWeapon = weapon;
        // GIVE с bForceInHand=0 доставание не гарантирует — латч не ставим.
        s_weaponEquippedInHands = false;
        s_weaponEquippedHash = 0;
        Init(natives::SET_PED_INFINITE_AMMO);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(1));
        Push(weapon);
        Call();
        Init(natives::_SET_PED_INFINITE_AMMO_CLIP);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(1));
        Call();
        sh_log::Write("coop: weapon 0x%llX re-given (was reset)\n", weapon);
    }
    // Латч: оружие уже в руках — SET_CURRENT_PED_WEAPON не дёргаем. Иначе
    // equipNow=1 на каждый aim/shoot (10-12 раз/с) переигрывает анимацию
    // доставания и сбивает TASK_SHOOT_AT_COORD/TASK_AIM_GUN_AT_COORD.
    if (s_weaponEquippedInHands && s_weaponEquippedHash == weapon)
        return;
    // Достать в руку ОДИН раз (equipNow=true) — без этого задача стрельбы
    // не срабатывает; латч сбрасывается только на события, которые реально
    // могут убрать оружие (бой/лошадь/смена/смерть).
    Init(natives::SET_CURRENT_PED_WEAPON);
    Push(s_ghost);
    Push(weapon);
    Push(static_cast<unsigned long long>(1)); // equipNow
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
    s_weaponEquippedInHands = true;
    s_weaponEquippedHash = weapon;
    sh_log::Write("coop: weapon 0x%llX equipped in hands\n", weapon);
}

// Прицеливание: задача «целиться в точку» (time = -1 — до смены задачи).
void AimGhostForward()
{
    EnsureGhostWeaponInHands();
    Vec3 t = GhostAimPointAhead();
    using namespace natives_api;
    Init(natives::TASK_AIM_GUN_AT_COORD);
    Push(s_ghost);
    Push(t.x); Push(t.y); Push(t.z);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // time (ms), -1
    Push(static_cast<unsigned long long>(0));          // p5 (instant blend)
    Push(static_cast<unsigned long long>(0));          // p6 (fire)
    Call();
}

// Стрельба: настоящие выстрелы в точку (движок сам даёт звук, вспышку,
// отдачу, гильзы). duration — сколько стрелять, SingleShot — темп.
void ShootGhostForward()
{
    EnsureGhostWeaponInHands();
    Vec3 t = GhostAimPointAhead();
    using namespace natives_api;
    Init(natives::TASK_SHOOT_AT_COORD);
    Push(s_ghost);
    Push(t.x); Push(t.y); Push(t.z);
    Push(static_cast<unsigned long long>(200));        // duration ms (~1 выстрел)
    Push(static_cast<unsigned long long>(0x5D60E4E0)); // SingleShot
    Push(static_cast<unsigned long long>(0));
    Call();
}

// Выстрел в КОНКРЕТНУЮ точку (из события Shot — точка попадания игрока).
void ShootGhostAt(const Vec3& t, int durationMs = 250)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    EnsureGhostWeaponInHands();
    using namespace natives_api;
    Init(natives::TASK_SHOOT_AT_COORD);
    Push(s_ghost);
    Push(t.x); Push(t.y); Push(t.z);
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(durationMs)));
    Push(static_cast<unsigned long long>(0x5D60E4E0)); // SingleShot
    Push(static_cast<unsigned long long>(0));
    Call();
}

// Применить действие (action) удалённого игрока к педу призрака.
// Вызывается из ApplyRemoteFrame (игровой поток).
// Прицеливание/стрельба/перезарядка — реальные задачи оружия: пед стоит,
// целится и стреляет (со звуком и вспышкой).
void ApplyActionToGhost(const net::FramePayload& f)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    const uint8_t ac = static_cast<uint8_t>(f.action);
    const bool wasCombat = (s_lastAction == static_cast<uint8_t>(net::Action::Shooting) ||
                            s_lastAction == static_cast<uint8_t>(net::Action::Reloading) ||
                            s_lastAction == static_cast<uint8_t>(net::Action::Aiming));
    const bool isCombat = (ac == static_cast<uint8_t>(net::Action::Shooting) ||
                           ac == static_cast<uint8_t>(net::Action::Reloading) ||
                           ac == static_cast<uint8_t>(net::Action::Aiming));
    s_combatActive = isCombat;

    // Выход из боя: снимаем боевую задачу и заставляем движение
    // перепоставить задачу на следующем кадре. Если только что был выстрел —
    // откладываем, чтобы не оборвать его до фактического срабатывания.
    if (wasCombat && !isCombat)
    {
        s_pendingCombatExit = true;
        s_lastAimTaskValid = false;
    }
    if (s_pendingCombatExit && s_shootHoldTicks == 0)
    {
        s_pendingCombatExit = false;
        Init(natives::TASK_STAND_STILL);
        Push(s_ghost); Push(static_cast<unsigned long long>(0xFFFFFFFF));
        Call();
        s_moveTargetValid = false;
        s_lastMoveState = 0xFF;
        // Выход из боя: движок может убрать оружие в кобуру — латч сбрасываем,
        // чтобы следующий выстрел один раз переэкипировал оружие.
        InvalidateGhostWeaponLatch();
    }

    // Перенаводка прицела: задача TASK_AIM_GUN_AT_COORD с time=-1 держится за
    // ФИКСИРОВАННОЙ точкой, а игрок продолжает вести прицел. Поэтому, пока
    // идёт прицеливание, обновляем задачу при сдвиге точки >0.5 м.
    if (isCombat && ac == static_cast<uint8_t>(net::Action::Aiming) &&
        s_shootHoldTicks == 0)
    {
        Vec3 t = GhostAimPointAhead();
        const bool moved = !s_lastAimTaskValid ||
            std::fabs(t.x - s_lastAimTaskPoint.x) > 0.5f ||
            std::fabs(t.y - s_lastAimTaskPoint.y) > 0.5f ||
            std::fabs(t.z - s_lastAimTaskPoint.z) > 0.5f;
        if (moved && (s_tickCount - s_lastAimTaskTick) >= 10)
        {
            AimGhostForward();
            s_lastAimTaskPoint = t;
            s_lastAimTaskValid = true;
            s_lastAimTaskTick = s_tickCount;
        }
    }

    // Пока стрельба продолжается — продлеваем задачу. Сериализация: пока
    // предыдущий выстрел доигрывает (hold), задачу НЕ перезадаём — иначе
    // анимация перезапускается и выстрел не «регистрируется».
    // ВАЖНО: если в очереди есть ТОЧНЫЕ события выстрелов (Shot из сети),
    // авто-стрельбу по action не делаем — иначе первый выстрел «съедает»
    // hold, а остальные события ждут и не проигрываются.
    if (isCombat && ac == static_cast<uint8_t>(net::Action::Shooting) &&
        s_shootHoldTicks == 0 &&
        !s_shotEventsPending.load() &&
        s_tickCount - s_lastShootTaskTick >= 15 &&
        s_tickCount - s_lastShotEventTaskTick >= 30)
    {
        s_lastShootTaskTick = s_tickCount;
        ShootGhostForward();
        s_shootHoldTicks = 30;
        return;
    }

    if (ac == s_lastAction) return;

    bool handled = true;
    switch (static_cast<net::Action>(ac))
    {
    case net::Action::InScenario:
    case net::Action::Sitting:
    case net::Action::Drunk:
        // Стоим на месте (не даём задач движения — пед не бежит во время
        // сценария). Физику не замораживаем: пед может стоять свободно.
        Init(natives::TASK_STAND_STILL);
        Push(s_ghost); Push(static_cast<unsigned long long>(0xFFFFFFFF));
        Call();
        sh_log::Write("coop: action=%d → stand still\n", (int)ac);
        break;
    case net::Action::Aiming:
        // Кадры «выстрел → прицел» идут вперемешку (IS_PED_SHOOTING живёт
        // 1-3 тика). Если сразу вернуть прицел, задача стрельбы отменяется
        // ДО выстрела. Поэтому держим выстрел ~0.5 с.
        if (s_shootHoldTicks > 0)
        {
            handled = false; // повторим прицел, когда выстрел отыграет
            break;
        }
        AimGhostForward();
        s_lastAimTaskPoint = GhostAimPointAhead();
        s_lastAimTaskValid = true;
        s_lastAimTaskTick = s_tickCount;
        sh_log::Write("coop: action=%d → aim\n", (int)ac);
        break;
    case net::Action::Shooting:
        // Пока предыдущий выстрел доигрывает — не перезадаём (анимация
        // перезапустилась бы, и выстрел потерялся бы).
        if (s_shootHoldTicks > 0)
        {
            handled = false;
            break;
        }
        s_lastShootTaskTick = s_tickCount;
        ShootGhostForward();
        s_shootHoldTicks = 30;
        sh_log::Write("coop: action=%d → shoot\n", (int)ac);
        break;
    case net::Action::Reloading:
        Init(natives::TASK_RELOAD_WEAPON);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(0));
        Call();
        sh_log::Write("coop: action=%d → reload\n", (int)ac);
        break;
    default:
        sh_log::Write("coop: action=%d → none\n", (int)ac);
        break;
    }
    if (handled)
        s_lastAction = ac;
}

// Применить особое состояние педа (PedState) и оружие/здоровье из кадра
// к педу призрака. Вызывается из Tick() (игровой поток), только при смене.
void ApplyPedStateToGhost(const net::FramePayload& f)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    const uint8_t st = f.state;

    if (st != s_lastPedState)
    {
        s_lastPedState = st;
        switch (static_cast<net::PedState>(st))
        {
        case net::PedState::Ragdoll:
            // Уронить педа в ragdoll.
            // SET_PED_TO_RAGDOLL: 7 аргументов (timeMin,timeMax,ragdollType,
            // abortIfInjured,abortIfDead,const char* taskName). Раньше
            // передавали 6 — движок читал указатель из мусора.
            Init(natives::SET_PED_TO_RAGDOLL);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(2000));  // timeMin (мс)
            Push(static_cast<unsigned long long>(4000));  // timeMax (мс)
            Push(static_cast<unsigned long long>(0));     // ragdollType
            Push(static_cast<unsigned long long>(0));     // abortIfInjured
            Push(static_cast<unsigned long long>(0));     // abortIfDead
            Push(reinterpret_cast<unsigned long long>(
                const_cast<char*>("")));                  // p6: имя NM-задачи
            Call();
            sh_log::Write("coop: state=Ragdoll applied\n");
            break;
        case net::PedState::InCover:
            // Стоим в укрытии не можем напрямую; минимум — не двигаемся задачей,
            // ставим stand-still (имитация укрытия позже).
            Init(natives::TASK_STAND_STILL);
            Push(s_ghost); Push(static_cast<unsigned long long>(0xFFFFFFFF));
            Call();
            sh_log::Write("coop: state=InCover (stand-still)\n");
            break;
        case net::PedState::Swimming:
        case net::PedState::SwimUnderwater:
            sh_log::Write("coop: state=Swimming (teleport to water pos only)\n");
            break;
        case net::PedState::Climbing:
            // Лезет: даём задачу лазания один раз (задача сама ведёт вдоль
            // препятствия; физику размораживаем, иначе TASK_CLIMB не сработает).
            SetGhostFrozen(false);
            Init(natives::TASK_CLIMB);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(0));
            Call();
            sh_log::Write("coop: state=Climbing -> TASK_CLIMB\n");
            break;
        case net::PedState::Jumping:
            // Прыжок: задача прыжка конфликтует с ручным lerp'ом позиции
            // (в воздухе), поэтому анимацию не задаём — пед плавно догоняет
            // точку кадра по всем осям (обрабатывается в ApplyRemoteFrame).
            sh_log::Write("coop: state=Jumping (lerp in air)\n");
            break;
        default:
            // Normal/Climbing/Jumping/Falling — движение обычное (move).
            sh_log::Write("coop: state=%d → normal move\n", (int)st);
            break;
        }
    }

    // Оружие: выдать педу и взять в руку при смене. Без GIVE_WEAPON_TO_PED
    // модель оружия не появляется (SET_CURRENT одного недостаточно), а
    // натив требует ПОЛНЫЙ набор параметров (13) — при меньшем числе движок
    // читает мусор и оружие «не появляется».
    if (f.weaponHash != s_lastWeapon)
    {
        s_lastWeapon = f.weaponHash;
        // weaponHash приходит из сети — в GIVE/SET_CURRENT пускаем только
        // валидный хэш (мусор/Unarmed игнорируем; текущее оружие не снимаем).
        const bool weaponValid = (f.weaponHash != 0) && IsWeaponHashValid(f.weaponHash);
        if (f.weaponHash && !weaponValid)
        {
            sh_log::Write("coop: ignoring invalid weapon hash 0x%llX\n",
                          f.weaponHash);
        }
        else if (weaponValid)
        {
            Init(natives::GIVE_WEAPON_TO_PED);
            Push(s_ghost);
            Push(f.weaponHash);
            Push(static_cast<unsigned long long>(60)); // ammoCount
            Push(static_cast<unsigned long long>(0));  // bForceInHand (0: с анимацией доставания)
            Push(static_cast<unsigned long long>(0));  // bForceInHolster
            Push(static_cast<unsigned long long>(0));  // attachPoint = HAND_PRIMARY
            Push(static_cast<unsigned long long>(0));  // bAllowMultipleCopies
            Push(0.5f);                                // p7 (как в R* скриптах)
            Push(1.0f);                                // p8
            Push(static_cast<unsigned long long>(0));  // addReason
            Push(static_cast<unsigned long long>(1));  // bIgnoreUnlocks
            Push(0.5f);                                // permanentDegradation
            Push(static_cast<unsigned long long>(0));  // p12
            Call();
            // Взять в руку (equipNow=true) — проигрывается доставание.
            Init(natives::SET_CURRENT_PED_WEAPON);
            Push(s_ghost);
            Push(f.weaponHash);
            Push(static_cast<unsigned long long>(1)); // equipNow
            Push(static_cast<unsigned long long>(0)); // attachPoint
            Push(static_cast<unsigned long long>(0));
            Push(static_cast<unsigned long long>(0));
            Call();
            s_givenWeapon = f.weaponHash;
            // SET_CURRENT_PED_WEAPON(equipNow=1) уже выполнен — ставим латч,
            // чтобы EnsureGhostWeaponInHands не переигрывал доставание.
            s_weaponEquippedInHands = true;
            s_weaponEquippedHash = f.weaponHash;
            // Бесконечные патроны: каждый реплей выстрела тратит боезапас, и
            // после ~60 выстрелов призрак замолкал (выстрелы «терялись»).
            Init(natives::SET_PED_INFINITE_AMMO);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(1));
            Push(f.weaponHash);
            Call();
            Init(natives::_SET_PED_INFINITE_AMMO_CLIP);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(1));
            Call();
            sh_log::Write("coop: weapon 0x%llX given+equipped (infinite ammo)\n",
                          f.weaponHash);
            // Доставание оружия сбрасывает component variation — тело педа
            // перестаёт рисоваться. Возвращаем внешний вид.
            RefreshGhostComponents();
            s_outfitRefreshTicks = 90;
        }
        else if (s_givenWeapon)
        {
            // Убрали оружие — снимаем его с педа (иначе остаётся в руке).
            Init(natives::REMOVE_WEAPON_FROM_PED);
            Push(s_ghost);
            Push(s_givenWeapon);
            Push(static_cast<unsigned long long>(0));
            Push(static_cast<unsigned long long>(0));
            Call();
            sh_log::Write("coop: weapon 0x%llX removed\n", s_givenWeapon);
            s_givenWeapon = 0;
            InvalidateGhostWeaponLatch();
            RefreshGhostComponents();
            s_outfitRefreshTicks = 60;
        }
    }

    // Действие (стрельба/сидение/сценарий…) — при смене.
    ApplyActionToGhost(f);
}

// Применить мета-наряд удалённого игрока к педу призрака (при смене хэша).
// Асинхронно: запрашиваем наряд, когда загрузится — применяем. Вызывается
// из Tick() (игровой поток).
void ApplyPlayerOutfitToGhost(unsigned long long outfitHash)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;

    if (!outfitHash)
    {
        s_wantedOutfit = 0;
        if (s_outfitRequestPending)
        {
            Init(natives::RELEASE_META_PED_OUTFIT_REQUEST);
            Push(static_cast<unsigned long long>(static_cast<int>(s_outfitRequest)));
            Call();
            s_outfitRequestPending = false;
        }
        return;
    }
    if (outfitHash == s_wantedOutfit && !s_outfitRequestPending) return;

    if (!s_outfitRequestPending)
    {
        // Запускаем запрос наряда. МОДЕЛЬ — та, что у педа-призрака СЕЙЧАС
        // (s_ghostModel: реальная модель персонажа из кадра reserved[2]).
        // Раньше использовали s_remoteModel («подставная» модель из ini) —
        // наряд запрашивался для неё, а применялся к педу с другой моделью
        // → применялся частично/рандомно (патронташи/кобуры/шляпы не те).
        unsigned long long model = s_ghostModel ? s_ghostModel
                                  : (s_remoteModel ? s_remoteModel : GHOST_PED_MODEL);
        Init(natives::REQUEST_META_PED_OUTFIT);
        Push(model);
        Push(outfitHash);
        s_outfitRequest = static_cast<int>(Call());
        s_outfitRequestPending = true;
        s_wantedOutfit = outfitHash;
        s_outfitRequestLogged = false;
        sh_log::Write("coop: outfit request 0x%llX (model 0x%llX) -> %d\n",
                      outfitHash, model, s_outfitRequest);
    }
    if (!s_outfitRequestPending) return;
    {
        Init(natives::HAS_META_PED_OUTFIT_LOADED);
        Push(static_cast<unsigned long long>(static_cast<int>(s_outfitRequest)));
        if (Call())
        {
            Init(natives::APPLY_PED_META_PED_OUTFIT);
            Push(static_cast<unsigned long long>(static_cast<int>(s_outfitRequest)));
            Push(s_ghost);
            Push(true); Push(true);
            bool ok = Call() != 0;
            sh_log::Write("coop: outfit applied to ghost, ok=%d\n", ok ? 1 : 0);
            Init(natives::RELEASE_META_PED_OUTFIT_REQUEST);
            Push(static_cast<unsigned long long>(static_cast<int>(s_outfitRequest)));
            Call();
            s_outfitRequestPending = false;
        }
        else if (!s_outfitRequestLogged)
        {
            sh_log::Write("coop: waiting outfit load...\n");
            s_outfitRequestLogged = true;
        }
    }
}

// Собрать компоненты одежды (наряд) локального игрока: список хэшей одетых
// компонентов (шляпа, куртка, рубашка, штаны, патронташ, кобура, пояс и т.п.)
// через _GET_NUM_COMPONENTS_IN_PED + _GET_PED_COMPONENT_CATEGORY_BY_INDEX.
// Возвращает false, если прочитать не удалось (насив вернул 0/нет данных).
void CollectLocalOutfit(unsigned int ped, net::OutfitState& out)
{
    using namespace natives_api;
    out.model = 0;
    out.count = 0;
    Init(natives::GET_ENTITY_MODEL); Push(ped);
    out.model = Call();

    // Правильный способ: _GET_NUM_COMPONENT_CATEGORIES_IN_PED + 
    // _GET_SHOP_ITEM_COMPONENT_AT_INDEX (входным struct'ом ждёт буфер-слоты).
    // GetShopItemComponents() из rdr2-runtime-scripthook делает именно так.
    Init(natives::GET_NUM_COMPONENT_CATEGORIES_IN_PED); Push(ped);
    int categories = static_cast<int>(Call());
    if (categories <= 0)
        return;
    // Диагностика: сколько категорий реально у педа (если > лимита — хвост
    // обрезался, и именно это давало рассинхрон одежды).
    {
        static int s_lastCats = -1;
        if (categories != s_lastCats)
        {
            s_lastCats = categories;
            sh_log::Write("coop: outfit categories for ped %u = %d (limit %d)\n",
                          ped, categories, net::kMaxOutfitComponents);
        }
    }
    if (categories > net::kMaxOutfitComponents)
        categories = net::kMaxOutfitComponents;

    // Два script-буфера по 4 слота (8 байт каждый) — их заполняет натив.
    // Для входа достаточно нулей (структуры «запроса»).
    static unsigned long long inA[4] = {0, 0, 0, 0};
    static unsigned long long inB[4] = {0, 0, 0, 0};

    int n = 0;
    for (int i = 0; i < categories; ++i)
    {
        std::memset(inA, 0, sizeof(inA));
        std::memset(inB, 0, sizeof(inB));
        Init(natives::GET_SHOP_ITEM_COMPONENT_AT_INDEX);
        Push(ped);
        Push(static_cast<unsigned long long>(i));
        Push(static_cast<unsigned long long>(1)); // p2 = true (как в rdr2-runtime)
        Push(reinterpret_cast<unsigned long long>(inA));
        Push(reinterpret_cast<unsigned long long>(inB));
        uint32_t comp = static_cast<uint32_t>(Call());
        if (comp)
            out.components[n++] = comp;
    }
    out.count = static_cast<uint32_t>(n);
}

// Одинаковые ли наборы компонентов (с учётом модели, если она известна).
// Сравниваем БЕЗ учёта порядка: движок отдаёт категории в разном порядке
// между кадрами/тиками, из-за чего наряд считался «изменённым»,
// переотправлялся и переприменялся (лишние слои одежды / случайный вид).
bool OutfitsEqual(const net::OutfitState& a, const net::OutfitState& b)
{
    if (a.count != b.count) return false;
    if (a.model && b.model && a.model != b.model) return false;
    if (a.count > static_cast<uint32_t>(net::kMaxOutfitComponents) ||
        b.count > static_cast<uint32_t>(net::kMaxOutfitComponents))
        return false;
    if (a.count == 0) return true;
    std::vector<uint32_t> ca(a.components, a.components + a.count);
    std::vector<uint32_t> cb(b.components, b.components + b.count);
    std::sort(ca.begin(), ca.end());
    std::sort(cb.begin(), cb.end());
    return ca == cb;
}

// Применить надетые shop-предметы (одежду) удалённого игрока к педу призрака
// через _APPLY_SHOP_ITEM_TO_PED + _UPDATE_PED_VARIATION (как в rdr2-runtime
// ApplyShopItemComponents). Вызывается из Tick() (игровой поток).
void ApplyOutfitToGhost(const net::OutfitState& os)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    // На труп одежду не надеваем: наряд применится при возрождении.
    if (s_ghostDead) return;
    // Модель педа должна совпадать (иначе компоненты от другой модели
    // применятся некорректно / визуально сломаются). Если пед ещё создан
    // со старой моделью (до пересоздания по кадру) — откладываем наряд.
    if (os.model && os.model != s_ghostModel)
    {
        s_pendingOutfit = os;
        s_havePendingOutfit = true;
        sh_log::Write("coop: outfit model mismatch (0x%llX != 0x%llX), deferred\n",
                      os.model, s_ghostModel);
        return;
    }
    // Уже надет этот же набор — пропускаем. Повторное применение
    // НАСЛАИВАЕТ одежду (нативный APPLY_SHOP_ITEM_TO_PED не заменяет, а
    // добавляет слой) → «сквозь одну одежду видно другую».
    if (s_appliedOutfitValid && OutfitsEqual(os, s_appliedOutfit))
        return;
    // Базовая вариация — ОДИН раз на педа (создание/респавн). Повторная
    // рандомизация при каждом применении наряда/refresh подмешивала случайные
    // компоненты (в категориях, где у оригинала пусто), и наряд расходился
    // с оригиналом. Индивидуальные shop-предметы применяем поверх.
    if (!s_ghostBaseRandomized)
    {
        RandomizePedComponents(s_ghost);
        s_ghostBaseRandomized = true;
    }
    SetEntityVisibleForce(s_ghost);
    for (uint32_t i = 0; i < os.count && i < net::kMaxOutfitComponents; ++i)
    {
        uint32_t c = os.components[i];
        if (!c) continue;
        Init(natives::APPLY_SHOP_ITEM_TO_PED);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(c));
        Push(static_cast<unsigned long long>(1)); // immediately = true
        Push(static_cast<unsigned long long>(0)); // isMp = false
        Push(static_cast<unsigned long long>(1)); // p4 = true (как в rdr2-runtime)
        Call();
    }
    Init(natives::UPDATE_PED_VARIATION);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(0)); // p1=false: не сбрасывать лицо/волосы
    Push(static_cast<unsigned long long>(1)); // p2=true
    Push(static_cast<unsigned long long>(1)); // p3=true
    Push(static_cast<unsigned long long>(1)); // p4=true
    Push(static_cast<unsigned long long>(0)); // p5=false
    Call();
    sh_log::Write("coop: outfit applied to ghost (%u shop items)\n", os.count);
    s_appliedOutfit = os;
    s_appliedOutfitValid = true;
}

// Пересобрать внешний вид педа после посадки на лошадь / спешивания: движок
// в этих переходах сбрасывает component variation, и модель педа перестаёт
// рисоваться (видны только надетыми одёжки). Пед при этом НЕ пересоздаётся —
// просто заново надеваем наряд на ту же сущность.
void RefreshGhostComponents()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    s_appliedOutfitValid = false;
    // Базу не рандомизируем повторно — только гарантируем, что variation
    // валидна (после посадки/оружия движок может её сбросить), и надеваем наряд.
    if (!s_ghostBaseRandomized)
    {
        RandomizePedComponents(s_ghost);
        s_ghostBaseRandomized = true;
    }
    SetEntityVisibleForce(s_ghost);
    if (s_outfitIn.count)
        ApplyOutfitToGhost(s_outfitIn);
}

// Применить отложенный наряд, если модель педа наконец совпала.
void TryApplyPendingOutfit()
{
    if (!s_havePendingOutfit) return;
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    if (s_pendingOutfit.model && s_pendingOutfit.model != s_ghostModel) return;
    net::OutfitState os = s_pendingOutfit;
    s_havePendingOutfit = false;
    ApplyOutfitToGhost(os);
}

// Применить снаряжение (седло/поклажу) лошади удалённого игрока к лошади-
// призраку. Мета-наряд у лошадей всегда 0, поэтому механизм тот же, что у
// одежды: APPLY_SHOP_ITEM_TO_PED + UPDATE_PED_VARIATION по shop-предметам.
void ApplyHorseOutfitToMount(const net::OutfitState& os)
{
    if (!s_mount || !DoesEntityExist(s_mount)) return;
    using namespace natives_api;

    if (!os.count)
    {
        s_horseOutfitAppliedValid = false;
        s_horseOutfitApplied = net::OutfitState{};
        return;
    }
    // Уже надет этот же набор — пропускаем (наслаивание предметов).
    if (s_horseOutfitAppliedValid && OutfitsEqual(os, s_horseOutfitApplied))
        return;
    // Сброс старого набора: RandomizePedComponents даёт валидную variation
    // (седло не «липнет» от старого), затем сразу новый полный набор.
    Init(natives::SET_PED_RANDOM_COMPONENT_VARIATION);
    Push(s_mount);
    Push(static_cast<unsigned long long>(0));
    Call();
    for (uint32_t i = 0; i < os.count && i < net::kMaxOutfitComponents; ++i)
    {
        uint32_t c = os.components[i];
        if (!c) continue;
        Init(natives::APPLY_SHOP_ITEM_TO_PED);
        Push(s_mount);
        Push(static_cast<unsigned long long>(c));
        Push(static_cast<unsigned long long>(1)); // immediately = true
        Push(static_cast<unsigned long long>(0)); // isMp = false
        Push(static_cast<unsigned long long>(1)); // p4 = true
        Call();
    }
    Init(natives::UPDATE_PED_VARIATION);
    Push(s_mount);
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(1));
    Push(static_cast<unsigned long long>(1));
    Push(static_cast<unsigned long long>(1));
    Push(static_cast<unsigned long long>(0));
    Call();
    s_horseOutfitApplied = os;
    s_horseOutfitAppliedValid = true;
    sh_log::Write("coop: horse outfit applied (%u shop items)\n", os.count);
}

// Применение позиции из сетевого кадра к педу призрака.
// Вызывается ИЗ Tick() (игровой поток).
// Движение ИНТЕРПОЛИРУЕТСЯ: за каждый тик пед смещается на часть пути к цели
// (без телепортов). Полный телепорт — только если скачок большой (спавн
// далеко от текущей точки, напр. из players.txt).
// Оживить педа-призрака, когда оригинал вернулся в мир после смерти.
// Если труп ещё существует — поднимаем его (RESURRECT_PED) и переносим к
// оригиналу. Если трупа уже нет (движок убрал) — заставляем спавн-блок
// создать педа заново. Плюс надпись «Player X is respawn» для проверки.
void RespawnGhostPed(const net::FramePayload* f)
{
    const char* who = s_remoteRespawnName[0] ? s_remoteRespawnName
                    : (s_pendingName[0] ? s_pendingName : "Player");
    using namespace natives_api;

    // Сброс состояний, чтобы задачи/наряд/действия/оружие применились заново.
    // ВАЖНО: раньше s_lastWeapon/s_givenWeapon не сбрасывались, и после
    // RESURRECT_PED (движок мог сбросить оружие) пед оставался без оружия в
    // руках — выстрелы молча не срабатывали. ResetGhostCombatState чистит и
    // это, и очередь выстрелов/hold.
    ResetGhostCombatState();
    s_ghostDead = false;
    s_deadGuardTicks = 0;
    s_reviveGraceTicks = 90; // ~1.5 с: не считаем педа мёртвым сразу
    s_lastMoveState = 0xFF;
    s_idleChase = false;
    s_appliedActionMode = 0;
    s_appliedScenario = 0;
    s_appliedTargetNetId = 0;
    s_lastLoggedTargetNetId = 0;
    s_lastCoverTaskTick = 0;
    s_moveTargetValid = false;
    // После RESURRECT_PED движок возвращает дефолтные компоненты модели,
    // поэтому наряд надо надеть заново (базу рандомизируем один раз).
    s_appliedOutfitValid = false;
    s_ghostBaseRandomized = false;
    // Игрок возродился в другом месте — старая лошадь уже не актуальна.
    if (s_mount)
        DismountAndRemoveMount();
    s_mountPhase = MountPhase::None;
    s_mountPhaseTicks = 0;
    s_rideLossTicks = 0;

    // Позиция возрождения: свежий кадр этого тика, иначе lastKnownPos (его
    // обновляет Tick на каждом кадре), а не позиция входа в сессию и не (0,0,0).
    Vec3 at{0.f, 0.f, 0.f};
    if (f)
        at = Vec3{f->x, f->y, f->z};
    else if (s_lastKnownPosValid)
        at = s_lastKnownPos;
    else if (s_ghost && DoesEntityExist(s_ghost))
        at = GetEntityCoords(s_ghost);
    else
        at = GetEntityCoords(GetPlayerPedFor(0));

    if (s_ghost && DoesEntityExist(s_ghost))
    {
        // Труп на месте — оживляем его.
        Init(natives::RESURRECT_PED);
        Push(s_ghost);
        Call();

        Init(natives::SET_ENTITY_HEALTH);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(200));
        Push(static_cast<unsigned long long>(0));
        Call();

        Init(natives::SET_ENTITY_INVINCIBLE);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(1));
        Call();

        Init(natives::SET_ENTITY_VISIBLE);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(1));
        Call();

        // Перенести к позиции оригинала (после респавна он в другом месте)
        // и поставить на землю.
        float groundZ = 0.f;
        const bool hasGround = GetGroundZForCoord(Vec3{at.x, at.y, at.z + 5.0f}, groundZ);
        if (hasGround)
            at.z = groundZ + 0.2f;
        SetEntityCoords(s_ghost, at);

        if (!s_blip)
            s_blip = CreateFriendlyBlipAt(at);
        if (!s_pedName[0] && s_pendingName[0])
            std::snprintf(s_pedName, sizeof(s_pedName), "%s", s_pendingName);
        if (s_outfitIn.count)
            RefreshGhostComponents(); // variation (1 раз) + наряд после воскрешения
        SetGhostStaminaFull();

        sh_log::Write("coop: ghost revived at (%.1f %.1f %.1f) ground=%d\n",
                      at.x, at.y, at.z, hasGround ? 1 : 0);
    }
    else
    {
        // Трупа нет — сбрасываем хэндл, спавн-блок создаст педа заново в
        // последней известной позиции.
        s_pendingHasPos = true;
        s_pendingX = at.x;
        s_pendingY = at.y;
        s_pendingZ = at.z;
        s_pendingHeading = f ? f->heading : s_lastKnownHeading;
        s_ghost = 0;
        sh_log::Write("coop: ghost corpse gone -> fresh respawn\n");
    }

    // Надпись для проверки возрождения.
    std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s is respawn", who);
    s_subtitleTicksLeft = 8 * 60;
    s_remoteRespawnName[0] = '\0';
}

void ApplyRemoteFrame(const net::FramePayload& f)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    // Мёртвый пед: труп лежит там, где умер — не двигаем. НО если оригинал
    // ожил (в кадре положительное здоровье) — воскрешаем педа. s_deadGuardTicks
    // не даёт кадру, отправленному ещё до смерти, воскресить труп мгновенно.
    if (s_ghostDead)
    {
        if (s_deadGuardTicks == 0 && f.health > 0.2f)
        {
            RespawnGhostPed(&f);
        }
        else
        {
            return; // всё ещё мёртв — труп не трогаем
        }
    }

    // P2: удалённый игрок едет в транспорте — позицией и посадкой управляет
    // PlayerVehicle-сообщение; кадры не должны телепортировать призрака.
    if (s_remoteInVehicle)
    {
        ApplyPedStateToGhost(f);
        ApplyPlayerOutfitToGhost(f.reserved[1]);
        return;
    }

    constexpr float LERP_FACTOR = 0.2f; // ~20% за тик при 60 Гц (плавно, без рывков)
    constexpr float TELEPORT_DIST = 10.0f; // больше этой дистанции — телепорт

    Vec3 cur = GetEntityCoords(s_ghost);
    Vec3 p{f.x, f.y, f.z};

    const uint8_t move = static_cast<uint8_t>(f.move);
    const uint8_t rideState = static_cast<uint8_t>(coop::MoveState::RideHorse);
    // Отсев мерцания: IS_PED_ON_MOUNT у отправителя может кратко дрожать,
    // поэтому «не верхом» считаем только после kRideLossTicks подряд.
    if (move == rideState)
        s_rideLossTicks = 0;
    else if (s_rideLossTicks < 1000)
        ++s_rideLossTicks;

    // Анимация спешивания: ждём, пока пед слезет, и только потом убираем
    // лошадь. Задач движения/состояний в это время не даём — прервут анимацию.
    if (s_mountPhase == MountPhase::Dismounting)
    {
        ++s_mountPhaseTicks;
        const bool off = !IsGhostMounted();
        // Лошадь убираем, когда пед уже слез и доиграл приземление
        // (~1.5 с), либо по жёсткому таймауту 4 с.
        if ((off && s_mountPhaseTicks >= 90) || s_mountPhaseTicks >= 240)
        {
            sh_log::Write("coop: dismount finished (off=%d, ticks=%u)\n",
                          off ? 1 : 0, s_mountPhaseTicks);
            DismountAndRemoveMount(); // удаляет лошадь + пересобирает внешний вид
        }
        return;
    }

    if (move == rideState)
    {
        // Верховая езда: педа напрямую НЕ двигаем и НЕ замораживаем —
        // он сидит на лошади; правим лошадь (спавн/движение/посадка).
        s_mountTarget = p;
        s_haveMountTarget = true;
        // Диагностика: лог прихода кадра с верховой ездой (редко — раз в
        // несколько секунд, чтобы не спамить лог при 15 кадрах/с).
        if (s_tickCount % (3 * 60) == 1)
            sh_log::Write("coop: frame RideHorse, mountWanted=0x%llX mount=%u "
                          "model=0x%llX outfit=0x%llX phase=%d\n",
                          s_mountWantedModel, s_mount,
                          f.reserved[0], f.horseOutfitHash,
                          static_cast<int>(s_mountPhase));
        // Модель коня из кадра (0 = стандартная). Хэш из сети валидируем:
        // невалидный не должен удалять текущую лошадь / запрашивать мусор.
        if (f.reserved[0] && f.reserved[0] != s_mountWantedModel)
        {
            if (!IsModelHashValid(f.reserved[0]))
            {
                if (f.reserved[0] != s_ignoredMountModelHash)
                {
                    s_ignoredMountModelHash = f.reserved[0];
                    sh_log::Write("coop: ignoring invalid mount model 0x%llX\n",
                                  f.reserved[0]);
                }
            }
            else
            {
                s_ignoredMountModelHash = 0;
                s_mountWantedModel = f.reserved[0];
                if (s_mount && DoesEntityExist(s_mount))
                {
                    DismountAndRemoveMount(); // модель сменилась — пересоздадим
                }
            }
        }
        if (!s_mount || !DoesEntityExist(s_mount))
        {
            // Идл-пед заморожен (FREEZE_ENTITY_POSITION) — задачу посадки на
            // замороженном педе движок не проигрывает. Размораживаем.
            SetGhostFrozen(false);
            RequestMountModel();       // создаёт лошадь рядом с педом
            if (s_mount)
                StartMounting();       // подойти и сесть с анимацией
            return;
        }
        if (s_mountPhase == MountPhase::Mounting)
        {
            ++s_mountPhaseTicks;
            if (IsGhostMounted())
            {
                s_mountPhase = MountPhase::Seated;
                s_mountTaskValid = false;
                sh_log::Write("coop: mounting finished (%u ticks)\n", s_mountPhaseTicks);
                // Снаряжение пришло по сети ДО создания лошади — применим
                // сейчас, когда пед уже сел (иначе седло так и не появится).
                if (s_horseOutfitIn.count)
                {
                    s_horseOutfitAppliedValid = false;
                    ApplyHorseOutfitToMount(s_horseOutfitIn);
                }
            }
            else if (s_mountPhaseTicks >= 6 * 60) // страховка ~6 с
            {
                sh_log::Write("coop: mounting timeout -> seat instantly\n");
                PromoteMountToGhost();
                s_mountPhase = MountPhase::Seated;
                s_mountTaskValid = false;
            }
            return; // пока пед садится — лошадь не двигаем
        }
        // В седле: ведём лошадь за кадрами.
        if (s_mountPhase != MountPhase::Seated)
        {
            s_mountPhase = MountPhase::Seated;
            s_mountTaskValid = false;
            // Если пед по каким-то причинам не в седле — сажаем. Иначе
            // лошадь уедет, а пед останется и будет «телепортироваться».
            if (!IsGhostMounted())
                PromoteMountToGhost();
        }
        MoveMountTo(p);
        // Пока лошадь ведёт задача, НЕ перебиваем heading каждый кадр:
        // TASK_FOLLOW сам разворачивает корпус, а ручной SetEntityHeading
        // дёргает лошадь на галопе. Heading правим только когда лошадь
        // почти на месте кадра (стоячая/медленная езда).
        {
            Vec3 mountPos = GetEntityCoords(s_mount);
            const float mdx = p.x - mountPos.x;
            const float mdy = p.y - mountPos.y;
            if (mdx * mdx + mdy * mdy < 1.5f * 1.5f)
                SetEntityHeading(s_mount, f.heading);
        }
        DisableMountLocalCollision();
        ApplyPedStateToGhost(f);
        return;
    }

    // Кадр «не верхом», но лошадь ещё есть.
    if (s_mount && DoesEntityExist(s_mount))
    {
        if (s_rideLossTicks >= kRideLossTicks)
            StartDismounting(); // запустит анимацию спешивания
        return;                 // мерцание — продолжаем держать посадку
    }

    // Прыжок/падение/плавание: пед в воздухе или в воде — Z кадра валиден
    // (это «пояс» игрока на высоте прыжка). НЕ прижимаем к земле и НЕ даём
    // задач движения (TASK_GO_* в прыжке заставляют педа «бежать по земле»
    // и хаотично дёргаться). Плавно догоняем точку кадра по ВСЕМ осям.
    const uint8_t st = static_cast<uint8_t>(f.state);
    if (st == static_cast<uint8_t>(net::PedState::Jumping) ||
        st == static_cast<uint8_t>(net::PedState::Falling) ||
        st == static_cast<uint8_t>(net::PedState::Swimming) ||
        st == static_cast<uint8_t>(net::PedState::SwimUnderwater))
    {
        const float STEP_AIR = 0.13f; // ~8 м/с при 60 Гц (было 0.35 для 15 Гц)
        float dx = p.x - cur.x, dy = p.y - cur.y, dz = p.z - cur.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > TELEPORT_DIST)
        {
            SetEntityCoords(s_ghost, p); // резкий перенос (спавн/загрузка)
        }
        else if (dist > 0.05f)
        {
            float s = std::min(STEP_AIR, dist);
            Vec3 n;
            n.x = cur.x + dx / dist * s;
            n.y = cur.y + dy / dist * s;
            n.z = cur.z + dz / dist * s;
            SetEntityCoords(s_ghost, n);
        }
        ApplyPedStateToGhost(f);
        return;
    }

    if (move == static_cast<uint8_t>(coop::MoveState::Walk) ||
        move == static_cast<uint8_t>(coop::MoveState::Run))
    {
        // Диагностика: редкий лог прихода кадра движения.
        if (s_tickCount % (3 * 60) == 1)
            sh_log::Write("coop: recv move=%u state=%u at (%.1f %.1f %.1f), cur=(%.1f %.1f %.1f)\n",
                          move, f.state, p.x, p.y, p.z, cur.x, cur.y, cur.z);
        // НЕ прижимаем педа к земле во время движения: TASK_FOLLOW_NAV_MESH
        // сам ведёт по навмешу (по земле). Ручной SetEntityCoords здесь —
        // источник багов: GET_GROUND_Z_FOR_3D_COORD на движущемся педе даёт
        // нестабильную высоту, пед «уходит под землю» и дёргается каждым
        // кадром (лог lowered спамил 40.7 -> 40.7). Прижим — только в Idle.
        // Движение задачей: не телепортируем и не лерпим — просто
        // перенаправляем задачу идти к текущей точке кадра (топает ножками).
        ApplyMoveToGhost(s_ghost, move, p);
    }
    else
    {
        // Idle: пед стоит задачей TASK_STAND_STILL. НЕ двигаем его руками
        // (шаг за тик) и НЕ дёргаем заморозкой — это и было причиной
        // «дёрганья в покое» (задача стоит, а мы телепаем координаты).
        // Только два исключения:
        //  1) цель очень далеко (>TELEPORT_DIST) — резкий перенос (загрузка);
        //  2) пед висит выше 0.5 м над землёй — опускаем на неё.
        // Покачивание корпуса в покое даёт микро-смещения цели. Если гонять
        // педа задачей к точке каждый раз, когда dist > 0.5 м, он «шаффлит»
        // ногами (Idle↔Walk). Поэтому — гистерезис:
        //   < CLOSE  — стоим (stand still);
        //   > RESUME — только тогда догоняем.
        const float CLOSE = 1.5f;
        const float RESUME = 2.5f;
        float dx = p.x - cur.x, dy = p.y - cur.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > TELEPORT_DIST)
        {
            SetEntityCoords(s_ghost, Vec3{p.x, p.y, cur.z});
        }
        if (dist > RESUME)
            s_idleChase = true;
        else if (dist < CLOSE)
            s_idleChase = false;

        if (s_idleChase)
        {
            // Пед реально отстал (спавн/остановился в другом месте) —
            // подводим задачей ходьбы.
            ApplyMoveToGhost(s_ghost,
                             static_cast<uint8_t>(coop::MoveState::Walk), p);
        }
        else
        {
            // Стоит рядом с целью: не трогаем XY, только прижим при подвесе.
            float groundZ = 0.f;
            const bool hasGround = GetGroundZForCoord(cur, groundZ);
            if (hasGround && (cur.z - groundZ) > 1.0f && groundZ > -100.f
                && (s_tickCount - s_lastIdleGroundFixTick) > 60)
            {
                // Опускаем на землю, если реально висим НАД ней >1 м.
                // Троттлинг (раз в сек): без него на неровной земле пед
                // «подтягивался» каждый кадр и дрожал.
                s_lastIdleGroundFixTick = s_tickCount;
                SetEntityCoords(s_ghost, Vec3{cur.x, cur.y, groundZ + 0.2f});
            }
            // Единый вызов для Idle: ставит/обновляет stand-still при смене
            // состояния (сам защищён s_lastMoveState — не дёргает).
            ApplyMoveToGhost(s_ghost, move, p);
        }
    }

    // Поворот интерполируем мягко: кратчайшая дуга. МЁРТВАЯ ЗОНА 2°:
    // не переписываем heading мелкими коррекциями каждый кадр — именно это
    // давало микро-дрожь стоящего педа (idle-анимация слегка смещает корпус,
    // а мы 15 раз/с «поправляли» поворот на 25% → пед трясся в покое).
    // ВАЖНО: во время прицеливания/стрельбы поворот НЕ трогаем — heading
    // тела игрока может не совпадать с направлением камеры, и наш SetEntityHeading
    // каждый кадр разворачивал педа «строго вперёд», перебивая прицел
    // (задачу TASK_AIM_GUN_AT_COORD). Ориентацией управляет боевая задача.
    if (!s_combatActive && s_shootHoldTicks == 0)
    {
        float h = f.heading - GetEntityHeading(s_ghost);
        while (h > 180.f) h -= 360.f;
        while (h < -180.f) h += 360.f;
        if (std::fabs(h) > 2.0f)
            SetEntityHeading(s_ghost, GetEntityHeading(s_ghost) + h * LERP_FACTOR);
    }

    // Применить особое состояние (ragdoll/cover/swim), оружие (при смене).
    ApplyPedStateToGhost(f);

    // Применить мета-наряд удалённого игрока (внешний вид) — при смене хэша.
    ApplyPlayerOutfitToGhost(f.reserved[1]);
}

// Физически удалить педа призрака из мира (без сброса имён/флагов —
// для пересоздания при смене модели персонажа).
void DeleteGhostEntity()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    // 1) Снимаем mission-флаг: помеченные mission-entity педы движок
    //    может не давать удалять.
    Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
    Push(s_ghost);
    Push(false);
    Push(false);
    Call();
    // 2) DELETE_PED принимает Ped* (указатель на handle!) — как REMOVE_BLIP.
    //    Передача handle значением молча НЕ удаляет педа.
    int h = static_cast<int>(s_ghost);
    Init(natives::DELETE_PED);
    Push(reinterpret_cast<unsigned long long>(&h));
    Call();
    // 3) Если пед всё ещё существует — доп. попытка через no-longer-needed.
    if (DoesEntityExist(s_ghost))
    {
        Init(natives::SET_ENTITY_AS_NO_LONGER_NEEDED);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
    }
    sh_log::Write("coop: ped %d deleted, exists now=%d\n",
                  s_ghost, DoesEntityExist(s_ghost) ? 1 : 0);
    s_ghost = 0;
}

// Удалить педа и метку, сбросить флаги спавна (игровой поток).
void CleanupGhost()
{
    DeleteGhostEntity();
    s_ghostDead = false;
    if (s_blip)
        s_blip = RemoveBlip(s_blip);
    s_playerJoinedNotified = false;
    s_subtitleTicksLeft = 0;
    s_joinedName[0] = '\0';
    s_pedName[0] = '\0';
    s_pendingName[0] = '\0';
    s_remoteModel = 0;
    s_pendingHasPos = false;
    s_requestedModel = 0;
    s_modelLoadedLogged = false;
    s_haveFrame = false;
    s_lastMoveState = 0xFF; // чтобы следующий пед не унаследовал задачу
    s_idleChase = false;
    ResetGhostCombatState();
    s_deadGuardTicks = 0;
    s_lastKnownPosValid = false;
    // P2: сброс Action v2 (новый пед/сессия).
    s_lastSentActionValid = false;
    s_appliedActionMode = 0;
    s_appliedScenario = 0;
    s_appliedTargetNetId = 0;
    s_lastLoggedTargetNetId = 0;
    s_lastCoverTaskTick = 0;
    s_hostBarrier = HostMissionBarrier{};
    s_guestBarrier = GuestMissionBarrier{};
    s_lastHostBarrierOfferTick = 0;
    s_lastGuestBarrierPollTick = 0;
    DeleteRemotePlayerVehicle();
    s_lastSentPvValid = false;
    {
        std::lock_guard<std::mutex> lk(s_actionMtx);
        s_haveActionIn = false;
    }
    {
        std::lock_guard<std::mutex> lk(s_pvMtx);
        s_havePvIn = false;
    }
    // Сброс дебаунса move (новый игрок — заново определяем состояние).
    s_moveStable = 0xFF;
    s_moveCandidate = 0xFF;
    s_moveCandidateCount = 0;
    // Сброс состояния мета-наряда (новый пед применит наряд заново).
    if (s_outfitRequestPending)
    {
        using namespace natives_api;
        Init(natives::RELEASE_META_PED_OUTFIT_REQUEST);
        Push(static_cast<unsigned long long>(static_cast<int>(s_outfitRequest)));
        Call();
        s_outfitRequestPending = false;
    }
    s_wantedOutfit = 0;
    s_outfitRequestLogged = false;
    // Сброс наряда: иначе новый игрок (тот же name/модель) получил бы наряд
    // предыдущего, а отложенный наряд применился бы к чужому педу.
    s_haveOutfit = false;
    s_outfitIn = net::OutfitState{};
    s_havePendingOutfit = false;
    s_pendingOutfit = net::OutfitState{};
    s_outfitRefreshTicks = 0;
    s_ghostBaseRandomized = false;
    s_haveCandidateOutfit = false;
    s_candidateOutfitTicks = 0;
    // Отправитель: после reconnect/нового пира наряд надо переслать заново.
    s_sentOutfitValid = false;
    s_sentHorseOutfitValid = false;
    s_lastHorseOutfitSendTick = 0;
    // Убрать лошадь (если удалённый игрок был верхом).
    if (s_mount)
        DismountAndRemoveMount();
    s_mountPhase = MountPhase::None;
    s_mountPhaseTicks = 0;
    s_rideLossTicks = 0;
    // Сброс состояния снаряжения лошади (новый игрок применит заново).
    s_haveHorseOutfit = false;
    s_horseOutfitIn = net::OutfitState{};
    s_horseOutfitApplied = net::OutfitState{};
    s_horseOutfitAppliedValid = false;
    // Новый пед — наряд нужно применить заново.
    s_appliedOutfitValid = false;
}

// Определить move-состояние локального игрока для кадра (передаётся
// удалённой стороне для выбора анимации педа). Приоритет: верховая езда >
// бег > ходьба > стояние.
uint8_t DetectLocalMoveState(unsigned int ped)
{
    using namespace natives_api;
    uint8_t raw;
    Init(natives::IS_PED_ON_MOUNT); Push(ped);
    if (Call())
    {
        raw = static_cast<uint8_t>(coop::MoveState::RideHorse);
    }
    else
    {
        Init(natives::IS_PED_RUNNING); Push(ped);
        if (Call())
        {
            raw = static_cast<uint8_t>(coop::MoveState::Run);
        }
        else
        {
            Init(natives::IS_PED_WALKING); Push(ped);
            bool walking = Call() != 0;
            // Фактическая скорость нужна и для fallback, и для гистерезиса.
            Init(natives::GET_ENTITY_SPEED); Push(ped);
            float speed = CallFloat();
            // Гистерезис (ключ к отсутствию дёрганья): из стояния (Idle) в
            // шаг переходим только при ЯВНОМ движении (>1.0 м/с), а из шага
            // в стояние — только когда точно встали (<0.3 м/с). Без этого
            // покачивание корпуса мерцало Walk<->Idle и пед дёргался.
            // Дополнительно: IS_PED_WALKING бывает истинным от дыхания/
            // микро-анимации покоя — требуем реальную скорость >0.35 м/с.
            const bool wasWalking =
                (s_moveStable == static_cast<uint8_t>(coop::MoveState::Walk));
            const float walkThresh = wasWalking ? 0.3f : 1.0f;
            const bool realWalking = walking && speed > 0.35f;
            raw = (realWalking || speed > walkThresh)
                    ? static_cast<uint8_t>(coop::MoveState::Walk)
                    : static_cast<uint8_t>(coop::MoveState::Idle);
        }
    }

    // Дебаунс: состояние меняем только после kMoveDebounce подряд одинаковых
    // замеров. Убирает мерцание Walk<->Idle (главная причина дёрганья педа).
    if (raw == s_moveStable)
    {
        s_moveCandidateCount = 0;
        s_moveCandidate = raw;
        return s_moveStable;
    }
    if (raw == s_moveCandidate)
    {
        ++s_moveCandidateCount;
    }
    else
    {
        s_moveCandidate = raw;
        s_moveCandidateCount = 1;
    }
    if (s_moveStable == 0xFF)
    {
        s_moveStable = raw; // первичная инициализация — сразу
        s_moveCandidateCount = 0;
        return s_moveStable;
    }
    if (s_moveCandidateCount >= kMoveDebounce)
    {
        s_moveStable = raw; // подтверждено — переключаем
        s_moveCandidateCount = 0;
    }
    return s_moveStable;
}

// Доп. состояние педа (PedState): ragdoll/укрытие/плавание/лестница/прыжок/
// падение. Приоритет сверху вниз.
uint8_t DetectLocalPedState(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_RAGDOLL); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::PedState::Ragdoll);
    Init(natives::IS_PED_SWIMMING_UNDER_WATER); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::PedState::SwimUnderwater);
    Init(natives::IS_PED_SWIMMING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::PedState::Swimming);
    Init(natives::IS_PED_IN_COVER); Push(ped); Push(true); Push(false);
    if (Call()) return static_cast<uint8_t>(net::PedState::InCover);
    Init(natives::IS_PED_CLIMBING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::PedState::Climbing);
    Init(natives::IS_PED_JUMPING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::PedState::Jumping);
    Init(natives::IS_PED_FALLING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::PedState::Falling);
    return static_cast<uint8_t>(net::PedState::Normal);
}

// Флаги локального игрока (sprint/weapon-ready).
uint8_t DetectLocalFlags(unsigned int ped)
{
    using namespace natives_api;
    uint8_t f = 0;
    Init(natives::IS_PED_SPRINTING); Push(ped);
    if (Call()) f |= net::kFlagSprinting;
    Init(natives::IS_PED_WEAPON_READY_TO_SHOOT); Push(ped);
    if (Call()) f |= net::kFlagWeaponReady;
    return f;
}

// Действие локального игрока (кратковременное/контекстное поведение).
// Приоритет сверху вниз: сценарий (едят/пьют/курят) > стрельба > перезарядка
// > сидение > прицеливание > пьян. Прыжок/падение/плавление/укрытие уже
// покрыты state. Возвращает 0 (Action::None) при обычном движении/стойке.
uint8_t DetectLocalAction(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_USING_ANY_SCENARIO); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::InScenario);
    Init(natives::IS_PED_SHOOTING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::Shooting);
    Init(natives::IS_PED_RELOADING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::Reloading);
    Init(natives::IS_PED_SITTING); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::Sitting);
    Init(natives::IS_PED_AIMING_FROM_COVER); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::Aiming);
    // Обычное прицеливание (не только из укрытия): оружие наготове к выстрелу.
    Init(natives::IS_PED_WEAPON_READY_TO_SHOOT); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::Aiming);
    Init(natives::IS_PED_DRUNK); Push(ped);
    if (Call()) return static_cast<uint8_t>(net::Action::Drunk);
    return static_cast<uint8_t>(net::Action::None);
}

// Стабильное действие локального игрока для отправки в кадре.
// Выстрел длится 1-3 тика, а кадр собирается раз в 4 тика — событие терялось.
// Поэтому стрельба/перезарядка «залипают» через latch, а остальные состояния
// проходят дебаунс (иначе aim моргал каждые 2 кадра из-за
// IS_PED_WEAPON_READY_TO_SHOOT, и пед-призрак дёргал задачи).
uint8_t s_localActionStable = 0;
uint8_t s_localActionCandidate = 0;
unsigned int s_localActionCandidateCount = 0;
unsigned int s_localShootLatch = 0;
unsigned int s_localReloadLatch = 0;
constexpr unsigned int kLocalShootLatchTicks = 8;   // ~0.13 с
constexpr unsigned int kLocalReloadLatchTicks = 20; // ~0.33 с
constexpr unsigned int kLocalActionDebounce = 5;

void UpdateLocalActionState(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_SHOOTING); Push(ped);
    if (Call()) s_localShootLatch = kLocalShootLatchTicks;
    else if (s_localShootLatch) --s_localShootLatch;

    Init(natives::IS_PED_RELOADING); Push(ped);
    if (Call()) s_localReloadLatch = kLocalReloadLatchTicks;
    else if (s_localReloadLatch) --s_localReloadLatch;

    uint8_t raw;
    if (s_localShootLatch)
        raw = static_cast<uint8_t>(net::Action::Shooting);
    else if (s_localReloadLatch)
        raw = static_cast<uint8_t>(net::Action::Reloading);
    else
    {
        Init(natives::IS_PED_USING_ANY_SCENARIO); Push(ped);
        if (Call()) raw = static_cast<uint8_t>(net::Action::InScenario);
        else
        {
            Init(natives::IS_PED_SITTING); Push(ped);
            if (Call()) raw = static_cast<uint8_t>(net::Action::Sitting);
            else
            {
                // Прицел определяем по игроку (free aim), а не по
                // IS_PED_WEAPON_READY_TO_SHOOT — он дрожит в покое.
                Init(natives::IS_PLAYER_FREE_AIMING); Push(0ull); // player 0
                if (Call()) raw = static_cast<uint8_t>(net::Action::Aiming);
                else
                {
                    Init(natives::IS_PED_DRUNK); Push(ped);
                    raw = Call() ? static_cast<uint8_t>(net::Action::Drunk)
                                 : static_cast<uint8_t>(net::Action::None);
                }
            }
        }
    }

    if (raw == s_localActionStable)
    {
        s_localActionCandidate = raw;
        s_localActionCandidateCount = 0;
        return;
    }
    if (raw == s_localActionCandidate) ++s_localActionCandidateCount;
    else { s_localActionCandidate = raw; s_localActionCandidateCount = 1; }

    // Стрельба/перезарядка — сразу (событие), остальное — после дебаунса.
    const bool immediate =
        raw == static_cast<uint8_t>(net::Action::Shooting) ||
        raw == static_cast<uint8_t>(net::Action::Reloading) ||
        s_localActionStable == static_cast<uint8_t>(net::Action::Shooting) ||
        s_localActionStable == static_cast<uint8_t>(net::Action::Reloading);
    if (immediate || s_localActionCandidateCount >= kLocalActionDebounce)
    {
        s_localActionStable = raw;
        s_localActionCandidate = raw;
        s_localActionCandidateCount = 0;
    }
}

// Точка прицела локального игрока (уходит в кадре) и событие выстрела.
float s_localAimX = 0.f, s_localAimY = 0.f, s_localAimZ = 0.f;
bool  s_localAimValid = false;
unsigned int s_lastShotEventTick = 0;

// Куда игрок целится: сущность под прицелом; иначе точка в 15 м впереди.
void UpdateLocalAim(unsigned int ped)
{
    using namespace natives_api;
    int target = 0;
    Init(natives::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT);
    Push(0ull); // player index
    Push(reinterpret_cast<unsigned long long>(&target));
    if (Call() && target && DoesEntityExist(static_cast<unsigned int>(target)))
    {
        Vec3 tc = GetEntityCoords(static_cast<unsigned int>(target));
        s_localAimX = tc.x;
        s_localAimY = tc.y;
        s_localAimZ = tc.z + 0.8f; // примерно грудь цели
        s_localAimValid = true;
        return;
    }
    // Игрок целится «в пустоту» (не в сущность): берём направление камеры —
    // иначе пед-призрак целится строго вперёд, а пуля летит по реальному
    // прицелу. Пед наводится туда, куда смотрит камера оригинал.
    {
        Init(natives::IS_PLAYER_FREE_AIMING);
        Push(0ull);
        if (Call())
        {
            Vec3 cam{0.f, 0.f, 0.f};
            Init(natives::GET_GAMEPLAY_CAM_COORD);
            CallVec3(cam);
            Vec3 rot{0.f, 0.f, 0.f};
            Init(natives::GET_GAMEPLAY_CAM_ROT);
            Push(static_cast<unsigned long long>(2)); // rotationOrder
            CallVec3(rot);
            // rotationOrder 2: x=pitch, y=roll, z=yaw (градусы).
            float pitch = rot.x * 0.01745329251f;
            float yaw = rot.z * 0.01745329251f;
            Vec3 dir{-std::sin(yaw) * std::cos(pitch),
                     std::cos(yaw) * std::cos(pitch),
                     std::sin(pitch)};
            s_localAimX = cam.x + dir.x * 30.0f;
            s_localAimY = cam.y + dir.y * 30.0f;
            s_localAimZ = cam.z + dir.z * 30.0f;
            s_localAimValid = true;
            return;
        }
    }
    Vec3 p = GetEntityCoords(ped);
    float h = GetEntityHeading(ped) * 0.01745329251f;
    s_localAimX = p.x - std::sin(h) * 15.0f;
    s_localAimY = p.y + std::cos(h) * 15.0f;
    s_localAimZ = p.z + 1.2f;
    s_localAimValid = true;
}

// Выстрел — отдельное событие (опрос состояния в кадре терял выстрелы).
// ОСНОВНОЙ детект — уменьшение патронов в магазине (GET_AMMO_IN_CLIP):
// он ловит и повторные попадания в одну точку, где impact coord не меняется.
// Fallback — изменение точки попадания / начало стрельбы (лук, метательное).
Vec3 s_lastImpact{0.f, 0.f, 0.f};
bool s_localWasShooting = false;
int  s_lastClipAmmo = -1;
unsigned long long s_lastClipWeapon = 0;
bool s_ammoQueryFailLogged = false;

// Реализация ниже (anonymous namespace), нужна здесь для детекта выстрела.
unsigned long long GetLocalWeaponHash(unsigned int ped);

void UpdateLocalShotEvents(unsigned int ped)
{
    using namespace natives_api;

    // 1) Надёжный детект: патроны в магазине уменьшились = был выстрел.
    const unsigned long long weapon = GetLocalWeaponHash(ped);
    int clipAmmo = 0;
    bool haveAmmo = false;
    if (weapon)
    {
        Init(natives::GET_AMMO_IN_CLIP);
        Push(ped);
        Push(reinterpret_cast<unsigned long long>(&clipAmmo));
        Push(weapon);
        haveAmmo = Call() != 0;
    }
    if (weapon && !haveAmmo && !s_ammoQueryFailLogged)
    {
        s_ammoQueryFailLogged = true;
        sh_log::Write("coop: GET_AMMO_IN_CLIP failed for weapon 0x%llX; "
                      "using impact/shooting fallback\n", weapon);
    }
    int shots = 0;
    if (haveAmmo)
    {
        if (weapon != s_lastClipWeapon || s_lastClipAmmo < 0 ||
            clipAmmo > s_lastClipAmmo) // смена оружия или перезарядка
        {
            s_lastClipWeapon = weapon;
            s_lastClipAmmo = clipAmmo;
        }
        else if (clipAmmo < s_lastClipAmmo)
        {
            shots = s_lastClipAmmo - clipAmmo;
            s_lastClipAmmo = clipAmmo;
            if (shots > 4) shots = 4; // дробь/дробовик — не спамим
        }
    }

    // 2) Точка попадания последнего выстрела (если натив дал данные).
    unsigned long long impactBuf[3] = {0, 0, 0};
    Vec3 impact{0.f, 0.f, 0.f};
    bool haveImpact = false;
    Init(natives::GET_PED_LAST_WEAPON_IMPACT_COORD);
    Push(ped);
    Push(reinterpret_cast<unsigned long long>(impactBuf));
    if (Call())
    {
        natives_api::ReadVec3Out(impactBuf, impact);
        haveImpact = true;
    }

    // 3) Fallback для оружия/бросков без магазина.
    Init(natives::IS_PED_SHOOTING); Push(ped);
    const bool shooting = Call() != 0;
    bool fallback = false;
    if (haveImpact)
    {
        if (impact.x != s_lastImpact.x || impact.y != s_lastImpact.y ||
            impact.z != s_lastImpact.z)
        {
            s_lastImpact = impact;
            fallback = true;
        }
    }
    if (shots == 0 && !fallback)
    {
        if (shooting && !s_localWasShooting)
            fallback = true;
        else if (!haveAmmo && shooting &&
                 s_tickCount - s_lastShotEventTick >= 8)
            fallback = true;
    }
    s_localWasShooting = shooting;

    int count = shots > 0 ? shots : (fallback ? 1 : 0);
    // Fallback не чаще ~30 Гц; реальные патроны шлём сразу (пачка выстрелов
    // за тик не теряется).
    if (shots == 0 && count > 0 && s_tickCount - s_lastShotEventTick < 2)
        count = 0;

    for (int k = 0; k < count; ++k)
    {
        s_lastShotEventTick = s_tickCount;
        net::ShotPayload sp{};
        if (haveImpact)
        {
            sp.x = impact.x; sp.y = impact.y; sp.z = impact.z;
        }
        else
        {
            sp.x = s_localAimX; sp.y = s_localAimY; sp.z = s_localAimZ;
        }
        g_netClient.SendShot(sp);
        sh_log::Write("coop: send shot (%.1f %.1f %.1f)%s\n",
                      sp.x, sp.y, sp.z, shots > 0 ? " [ammo]" : " [fallback]");
    }
}

// Здоровье локального педа 0..1 (GET_ENTITY_HEALTH / max примерно 100..200 —
// нормируем).
float GetLocalHealth01(unsigned int ped)
{
    using namespace natives_api;
    // GET_ENTITY_HEALTH возвращает int (не float!). CallFloat() читал младшие
    // 32 бита int как float → health всегда ~0, и авто-возрождение призрака
    // по кадру (f.health > 0.2) никогда не срабатывало.
    Init(natives::GET_ENTITY_HEALTH); Push(ped);
    int h = static_cast<int>(Call());
    if (h < 0) h = 0;
    if (h > 200) h = 200; // нормализация как раньше (0..200 -> 0..1)
    return static_cast<float>(h) / 200.f;
}

// Применить move-состояние к педу призрака (вызывается из Tick, игровой поток).
// Для движения ставим задачу идти в точку кадра (пед топает ножками, а не
// телепортируется); для стояния — TASK_STAND_STILL. Пустая/стабильная задача
// НЕ ставится каждый тик (избегаем дёрганья) — применяем при смене состояния.
void ApplyMoveToGhost(unsigned int ped, uint8_t move, const Vec3& target)
{
    using namespace natives_api;
    const uint8_t runState = static_cast<uint8_t>(coop::MoveState::Run);
    const uint8_t walkState = static_cast<uint8_t>(coop::MoveState::Walk);
    // Во время прицеливания/стрельбы/перезарядки пед стоит и держит оружие.
    // Любая задача движения (TASK_FOLLOW/TASK_STAND_STILL) отменила бы
    // боевую задачу (прицел/выстрел) — поэтому движение не трогаем.
    // s_shootHoldTicks учитываем тоже: кадр с action=None приходит с задержкой
    // и сбрасывал combat раньше, чем SingleShot успевал выстрелить.
    if (s_combatActive || s_shootHoldTicks > 0)
    {
        Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
        Push(ped);
        Push(0.0f);
        Call();
        s_lastMoveState = move;
        return;
    }
    // Диагностика: смена состояния движения (если пед дёргается — тут будет
    // видно мерцание Walk<->Idle).
    if (move != s_lastMoveState)
        sh_log::Write("coop: move change %u -> %u\n", (unsigned)s_lastMoveState, (unsigned)move);
    if (move == walkState || move == runState)
    {
        // Движение задачей (топает ножками). Физику педа НЕ замораживаем,
        // иначе TASK_GO_* не сможет его вести.
        // ВАЖНО: размораживаем на КАЖДОМ кадре движения, а не только при
        // смене состояния. Иначе после Idle (заморожен) с продолжением Walk
        // пед остаётся замороженным — задача ведёт, но физика стоит, и он
        // «застревает» на месте, пока ручной SetEntityCoords не телепортнёт.
        SetGhostFrozen(false);
        if (s_lastMoveState != move)
        {
            // При переходе ОТ Idle/стояния к движению цель устарела — задачу
            // надо поставить заново. При смене Walk<->Run цель НЕ трогаем:
            // повторная постановка TASK_FOLLOW вызывает «подхват» педа
            // (телепорт ~1 м вперёд при переходе на бег).
            if (s_lastMoveState != walkState && s_lastMoveState != runState)
                s_moveTargetValid = false;
            s_lastMoveState = move;
        }
        // Ограничить скорость АНИМАЦИИ. Шкала RDR2: 0 = стоим, 1.0 = шаг,
        // 2.0 = бег, 3.0 = спринт. Раньше стояло 2.0 для шага — это уровень
        // БЕГА, поэтому пед при шаге оригинала срывался в бег (прерывисто).
        {
            const float maxBlend = (move == runState) ? 2.0f : 1.0f;
            Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
            Push(ped);
            Push(maxBlend);
            Call();
        }
        // Считаем, нужно ли ПЕРЕдавать задачу. При движении перезадаём, если
        // цель ушла дальше 0.25 м от предыдущей (меньше — пед точнее следует
        // за игроком; больше — реже дёргает задачу). Смена Walk<->Run сама по
        // себе НЕ перезадаёт задачу (движение уже идёт; повторная постановка
        // TASK_FOLLOW вызывает «подхват» педа — телепорт ~1 м вперёд при
        // переходе на бег). Скорость бега задаётся SET_PED_MAX_MOVE_BLEND_RATIO
        // выше, а задача продолжает вести педа к той же цели.
        // stoppingRange 0.1 — задача гасится у самой цели, пед почти не
        // «недохаживает» (люфт следа ~0.25 м, это меньше, чем был).
        bool retarget = (!s_moveTargetValid ||
                         std::fabs(target.x - s_moveTarget.x) > 0.25f ||
                         std::fabs(target.y - s_moveTarget.y) > 0.25f);

        if (retarget)
        {
            // TASK_FOLLOW_NAV_MESH_TO_COORD — 5-й параметр это
            // speedMultiplier (множитель, НЕ м/с): 1.0 = обычный шаг,
            // 2.0 = бег. Раньше стояло 2.5 для шага — пед бежал.
            const float speed = (move == runState) ? 2.0f : 1.0f;
            Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
            Push(ped);
            Push(target.x); Push(target.y); Push(target.z);
            Push(speed);                    // speedMultiplier (шаг 1.0 / бег 2.0)
            Push(static_cast<unsigned long long>(0xFFFFFFFF)); // timeout = -1
            Push(0.1f);                     // stoppingRange — задача гасится у цели
            Push(static_cast<unsigned long long>(4)); // flags
            Push(40000.f);                  // как в rdr2-runtime
            Call();
            s_moveTarget = target;
            s_moveTargetValid = true;
        }
        s_lastMoveState = move;
    }
    else // Idle / RideHorse (верховую анимацию не делаем, просто стоим)
    {
        // Сброс «скорости анимации движения»: иначе движок продолжает
        // проигрывать бег/отдышку на месте (педа как будто «прёт» стоя).
        Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
        Push(ped);
        Push(0.0f);
        Call();

        if (s_lastMoveState != move)
        {
            // Стоящий пед: ставим stand-still, НО НЕ замораживаем физику.
            // Заморозка (FREEZE_ENTITY_POSITION) конфликтует с задачей и
            // ручным позиционированием — пед «дёргается» (задача ведёт,
            // физика стоит). Свободная физика + TASK_STAND_STILL = стоит ровно.
            s_lastMoveState = move;
            Init(natives::TASK_STAND_STILL);
            Push(ped); Push(static_cast<unsigned long long>(0xFFFFFFFF));
            Call();
            // Восстановить выносливость — чтобы пед не отдувался после бега.
            SetGhostStaminaFull();
        }
    }
}

// Хэш оружия в руке игрока (GET_CURRENT_PED_WEAPON), 0 = нет.
// ВАЖНО: у натива 5 параметров (ped, &hash, p2, attachPoint, p4) — при
// меньшем числе движок читает мусор и оружие не синхронизировалось.
unsigned long long GetLocalWeaponHash(unsigned int ped)
{
    using namespace natives_api;
    unsigned long long out = 0;
    Init(natives::GET_CURRENT_PED_WEAPON);
    Push(ped);
    Push(reinterpret_cast<unsigned long long>(&out));
    Push(static_cast<unsigned long long>(1)); // p2 (как в rdr2-runtime)
    Push(static_cast<unsigned long long>(0)); // attachPoint = HAND_PRIMARY
    Push(static_cast<unsigned long long>(0)); // p4
    Call();
    // Unarmed (0xA2719263) — это «без оружия», а не предмет.
    if (out == 0xA2719263ull)
        out = 0;
    return out;
}

// Обработка входящего кадра: модель персонажа удалённого игрока (reserved[2]).
// Если отличается от текущей модели педа — пересоздаём педа (удаляем старый,
// новый заспавнится в Tick по wantModel). Модель 0 — не трогаем.
void CheckRemoteModelChange(const net::FramePayload& f)
{
    if (!f.reserved[2] || f.reserved[2] == s_ghostModel) return;
    // Модель пришла из сети: невалидный хэш НЕ должен приводить к удалению
    // текущего педа и RequestModel (иначе HasModelLoaded()==false навсегда и
    // Tick перестаёт обрабатывать сеть). Невалидную модель просто игнорируем.
    if (!IsModelHashValid(f.reserved[2]))
    {
        if (f.reserved[2] != s_ignoredPedModelHash)
        {
            s_ignoredPedModelHash = f.reserved[2];
            sh_log::Write("coop: ignoring invalid remote model 0x%llX\n",
                          f.reserved[2]);
        }
        return;
    }
    s_ignoredPedModelHash = 0;
    sh_log::Write("coop: remote model changed 0x%llX -> 0x%llX\n",
                  s_ghostModel, f.reserved[2]);
    // Запоминаем новую модель и просим её загрузить (в Tick чуть ниже будет
    // использована при спавне). Старого педа удаляем — новый появится сразу.
    s_requestedModel = f.reserved[2];
    RequestModel(f.reserved[2]);
    s_ghostModel = f.reserved[2];
    DeleteGhostEntity();
    // Сброс состояния наряда: старый запрос относился к прежней модели и
    // старому педу. Новый пед применит наряд заново (по новой модели).
    if (s_outfitRequestPending)
    {
        using namespace natives_api;
        Init(natives::RELEASE_META_PED_OUTFIT_REQUEST);
        Push(static_cast<unsigned long long>(static_cast<int>(s_outfitRequest)));
        Call();
        s_outfitRequestPending = false;
    }
    s_wantedOutfit = 0;
    s_outfitRequestLogged = false;
    // Пед будет создан заново — наряд применяем с чистого листа (базу
    // рандомизируем один раз), отложенный наряд старой модели отбрасываем.
    s_appliedOutfitValid = false;
    s_ghostBaseRandomized = false;
    s_havePendingOutfit = false;
    s_pendingOutfit = net::OutfitState{};
    s_outfitRefreshTicks = 0;
    // Пед удалён — лошадь-призрак без седока не нужна.
    if (s_mount)
        DismountAndRemoveMount();
    s_mountPhase = MountPhase::None;
    s_mountPhaseTicks = 0;
    s_rideLossTicks = 0;
    if (s_blip)
        s_blip = RemoveBlip(s_blip);
    s_ghostDead = false;
    s_lastMoveState = 0xFF;
    s_idleChase = false;
    ResetGhostCombatState(); // новому педу оружие выдадим заново
    s_deadGuardTicks = 0;
    // Педа пересоздали — применённое действие больше не актуально.
    s_appliedActionMode = 0;
    s_appliedScenario = 0;
    s_appliedTargetNetId = 0;
    s_lastLoggedTargetNetId = 0;
    s_lastCoverTaskTick = 0;
    s_hostBarrier = HostMissionBarrier{};
    s_guestBarrier = GuestMissionBarrier{};
    s_lastHostBarrierOfferTick = 0;
    s_lastGuestBarrierPollTick = 0;
    DeleteRemotePlayerVehicle();
    s_lastSentPvValid = false;
    {
        std::lock_guard<std::mutex> lk(s_actionMtx);
        s_haveActionIn = false;
    }
    {
        std::lock_guard<std::mutex> lk(s_pvMtx);
        s_havePvIn = false;
    }
    // НОВЫЙ пед создаётся со спавна (рядом с хостом / на сохранённой позиции);
    // наряд из последнего кадра применится автоматически (reserved[1]).
}

// P2: Action v2 — наблюдение действий локального игрока. Клип/фазу AnimGraph
// публичный SDK не отдаёт, поэтому передаём сценарий/укрытие/переноску и т.п.
net::ActionStatePayload DetectLocalActionV2(unsigned int ped)
{
    using namespace natives_api;
    net::ActionStatePayload a{};

    // Сценарий: сидение/курение/еда/питьё/лагерные действия и т.п.
    uint32_t scenario = 0;
    Init(natives::_GET_ACTIVE_DYNAMIC_SCENARIO); Push(ped);
    scenario = static_cast<uint32_t>(Call());
    if (!scenario)
    {
        Init(natives::_GET_ACTIVE_DYNAMIC_SCENARIO_2); Push(ped);
        scenario = static_cast<uint32_t>(Call());
    }
    if (scenario)
    {
        a.mode = static_cast<uint8_t>(net::ActionMode::Scenario);
        a.scenarioHash = scenario;
    }

    Init(natives::IS_PED_IN_COVER); Push(ped); Push(true); Push(false);
    if (Call())
    {
        if (a.mode == 0) a.mode = static_cast<uint8_t>(net::ActionMode::Cover);
        a.flags |= net::kActionFlagInCover;
    }
    Init(natives::IS_PED_AIMING_FROM_COVER); Push(ped);
    if (Call()) a.flags |= net::kActionFlagAiming;

    Init(natives::IS_PED_CARRYING_SOMETHING); Push(ped);
    if (Call())
    {
        a.mode = static_cast<uint8_t>(net::ActionMode::Carry);
        a.flags |= net::kActionFlagCarrying;
        // Цель переноски — тело/предмет. netId нужен гостю, чтобы взять прокси.
        Init(natives::_GET_FIRST_ENTITY_PED_IS_CARRYING); Push(ped);
        const unsigned int carried = static_cast<unsigned int>(Call());
        if (carried)
        {
            uint64_t nid = 0;
            if (world_sync::GetNetIdForHandle(carried, nid))
                a.targetNetId = static_cast<uint32_t>(nid);
        }
    }
    Init(natives::_IS_PED_DRAGGING); Push(ped);
    if (Call())
    {
        a.mode = static_cast<uint8_t>(net::ActionMode::Drag);
        a.flags |= net::kActionFlagDragging;
    }
    Init(natives::IS_PED_HOGTIED); Push(ped);
    if (Call())
    {
        a.mode = static_cast<uint8_t>(net::ActionMode::Hogtie);
        a.flags |= net::kActionFlagHogtied;
    }
    // Лутание: цель — труп/контейнер. netId даст гостю прокси для TASK_LOOT_ENTITY.
    Init(natives::GET_LOOTING_PICKUP_TARGET_ENTITY); Push(ped);
    const unsigned int lootTarget = static_cast<unsigned int>(Call());
    if (lootTarget)
    {
        a.mode = static_cast<uint8_t>(net::ActionMode::Loot);
        uint64_t nid = 0;
        if (world_sync::GetNetIdForHandle(lootTarget, nid))
            a.targetNetId = static_cast<uint32_t>(nid);
    }
    unsigned long long fishingOut = 0;
    Init(natives::_GET_TASK_FISHING); Push(ped);
    Push(reinterpret_cast<unsigned long long>(&fishingOut));
    if (Call())
    {
        a.mode = static_cast<uint8_t>(net::ActionMode::Fishing);
        a.flags |= net::kActionFlagFishing;
    }
    return a;
}

// P2: применить действие удалённого игрока к педу-призраку.
void ApplyRemoteActionV2(const net::ActionStatePayload& a)
{
    if (!s_ghost || !DoesEntityExist(s_ghost) || s_ghostDead)
        return;
    using namespace natives_api;

    const uint8_t mode = a.mode;
    const uint8_t scenarioMode = static_cast<uint8_t>(net::ActionMode::Scenario);
    const uint8_t coverMode    = static_cast<uint8_t>(net::ActionMode::Cover);
    const uint8_t carryMode    = static_cast<uint8_t>(net::ActionMode::Carry);
    const uint8_t lootMode     = static_cast<uint8_t>(net::ActionMode::Loot);
    const bool wasScenario = (s_appliedActionMode == scenarioMode);
    const bool wasCover = (s_appliedActionMode == coverMode);
    const bool wasCarryOrLoot =
        (s_appliedActionMode == carryMode || s_appliedActionMode == lootMode);

    // 1) Сначала завершаем ПРЕДЫДУЩЕЕ состояние: иначе его exit-задача
    //    (или TASK_STAND_STILL) перебьёт только что применённое новое действие.
    if (wasCover && mode != coverMode)
    {
        Init(natives::TASK_EXIT_COVER);
        Push(s_ghost);
        Call();
        sh_log::Write("action: cover exit\n");
    }
    if (wasScenario && mode != scenarioMode)
    {
        Init(natives::TASK_STAND_STILL);
        Push(s_ghost); Push(static_cast<unsigned long long>(0xFFFFFFFF));
        Call();
        sh_log::Write("action: scenario finished -> stand still\n");
    }
    if (wasCarryOrLoot && mode != carryMode && mode != lootMode)
    {
        Init(natives::TASK_STAND_STILL);
        Push(s_ghost); Push(static_cast<unsigned long long>(0xFFFFFFFF));
        Call();
        s_appliedTargetNetId = 0;
        sh_log::Write("action: carry/loot finished -> stand still\n");
    }

    // 2) Применяем текущее состояние.
    if (mode == scenarioMode && a.scenarioHash)
    {
        if (s_appliedActionMode != mode || s_appliedScenario != a.scenarioHash)
        {
            const float heading = s_haveFrame ? s_lastFrame.heading
                                              : GetEntityHeading(s_ghost);
            Init(natives::TASK_START_SCENARIO_IN_PLACE_HASH);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(a.scenarioHash));
            Push(static_cast<unsigned long long>(0xFFFFFFFF)); // duration -1
            Push(static_cast<unsigned long long>(1));          // playEnterAnim
            Push(static_cast<unsigned long long>(0));          // conditionalHash
            Push(heading);
            Push(static_cast<unsigned long long>(0));
            Call();
            sh_log::Write("action: scenario 0x%08X applied to ghost\n", a.scenarioHash);
        }
    }

    if (mode == coverMode)
    {
        if (!wasCover)
        {
            Init(natives::TASK_ENTER_COVER);
            Push(s_ghost);
            Call();
            s_lastCoverTaskTick = s_tickCount;
            sh_log::Write("action: cover enter\n");
        }
        else if (!s_combatActive && (s_tickCount - s_lastCoverTaskTick) > 90)
        {
            Init(natives::IS_PED_IN_COVER); Push(s_ghost); Push(true); Push(false);
            if (!Call())
            {
                Init(natives::TASK_ENTER_COVER);
                Push(s_ghost);
                Call();
                sh_log::Write("action: cover re-enter (task lost)\n");
            }
            s_lastCoverTaskTick = s_tickCount;
        }
    }

    if (mode == carryMode || mode == lootMode)
    {
        unsigned int target = 0;
        const bool resolved = a.targetNetId &&
            world_sync::GetHandleForNetId(a.targetNetId, target) &&
            target && target != s_ghost;
        if (resolved && a.targetNetId != s_appliedTargetNetId)
        {
            if (mode == carryMode)
            {
                Init(natives::TASK_PICKUP_CARRIABLE_ENTITY);
                Push(s_ghost); Push(target);
                Call();
                sh_log::Write("action: carry target netId=%u handle=%u\n",
                              a.targetNetId, target);
            }
            else
            {
                Init(natives::TASK_LOOT_ENTITY);
                Push(s_ghost); Push(target);
                Call();
                sh_log::Write("action: loot target netId=%u handle=%u\n",
                              a.targetNetId, target);
            }
            s_appliedTargetNetId = a.targetNetId;
        }
        else if (!resolved && a.targetNetId &&
                 a.targetNetId != s_lastLoggedTargetNetId)
        {
            s_lastLoggedTargetNetId = a.targetNetId;
            sh_log::Write("action: target netId=%u not found on guest "
                          "(world_sync must be enabled on both)\n", a.targetNetId);
        }
    }

    s_appliedActionMode = mode;
    s_appliedScenario = a.scenarioHash;
}

// P2: посадка в транспорт. Локальный детект: если игрок внутри vehicle —
// отправляем модель/позицию/место; гость создаёт прокси и сажает призрака.
net::PlayerVehicleState DetectLocalPlayerVehicle(unsigned int ped)
{
    using namespace natives_api;
    net::PlayerVehicleState pv{};
    Init(natives::GET_VEHICLE_PED_IS_IN);
    Push(ped); Push(false);
    const unsigned int veh = static_cast<unsigned int>(Call());
    if (!veh)
        return pv;

    Init(natives::GET_ENTITY_MODEL); Push(veh);
    pv.vehicleModel = static_cast<unsigned int>(Call());
    if (!pv.vehicleModel)
        return net::PlayerVehicleState{};

    const Vec3 pos = GetEntityCoords(veh);
    pv.x = pos.x; pv.y = pos.y; pv.z = pos.z;
    pv.heading = GetEntityHeading(veh);
    pv.flags = net::kPlayerVehicleFlagValid;

    // Ищем реальное место: -1 = водитель, дальше 0..4.
    int seat = -1;
    for (int s = -1; s <= 4; ++s)
    {
        Init(natives::GET_PED_IN_VEHICLE_SEAT);
        Push(veh); Push(s);
        if (static_cast<unsigned int>(Call()) == ped)
        {
            seat = s;
            break;
        }
    }
    pv.seat = static_cast<int8_t>(seat);
    if (seat == -1)
        pv.flags |= net::kPlayerVehicleFlagDriver;
    return pv;
}

void DeleteRemotePlayerVehicle()
{
    if (s_playerVehicle)
    {
        using namespace natives_api;
        int h = static_cast<int>(s_playerVehicle);
        Init(natives::DELETE_VEHICLE);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
        if (DoesEntityExist(s_playerVehicle))
        {
            int h2 = static_cast<int>(s_playerVehicle);
            Init(natives::SET_ENTITY_AS_NO_LONGER_NEEDED);
            Push(reinterpret_cast<unsigned long long>(&h2));
            Call();
        }
        sh_log::Write("vehicle: proxy deleted\n");
    }
    s_playerVehicle = 0;
    s_playerVehicleModel = 0;
    s_remoteInVehicle = false;
}

void ApplyRemotePlayerVehicle(const net::PlayerVehicleState& pv)
{
    using namespace natives_api;
    if (!(pv.flags & net::kPlayerVehicleFlagValid) || !pv.vehicleModel)
    {
        if (s_remoteInVehicle || s_playerVehicle)
        {
            DeleteRemotePlayerVehicle();
            sh_log::Write("vehicle: remote exit\n");
        }
        return;
    }
    if (!s_ghost || !DoesEntityExist(s_ghost) || s_ghostDead)
        return;

    // Игрок в транспорте — лошадь-призрак больше не нужна.
    if (s_mount && DoesEntityExist(s_mount))
        DismountAndRemoveMount();

    if (!s_playerVehicle || !DoesEntityExist(s_playerVehicle) ||
        s_playerVehicleModel != pv.vehicleModel)
    {
        DeleteRemotePlayerVehicle();
        if (!HasModelLoaded(pv.vehicleModel))
        {
            RequestModel(pv.vehicleModel);
            return;
        }
        Init(natives::CREATE_VEHICLE);
        Push(static_cast<unsigned long long>(pv.vehicleModel));
        Push(pv.x); Push(pv.y); Push(pv.z); Push(pv.heading);
        Push(false); // isNetwork
        Push(false); // bScriptHostVeh
        Push(true);  // bDontAutoCreateDraftAnimals
        Push(false); // p8
        const unsigned int veh = static_cast<unsigned int>(Call());
        if (!veh)
            return;
        Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
        Push(veh); Push(true); Push(true); Call();
        Init(natives::SET_ENTITY_INVINCIBLE);
        Push(veh); Push(true); Call();
        Init(natives::SET_ENTITY_VISIBLE);
        Push(veh); Push(true); Call();
        Init(natives::SET_ENTITY_COLLISION);
        Push(veh); Push(false); Push(false); Call();
        s_playerVehicle = veh;
        s_playerVehicleModel = pv.vehicleModel;
        sh_log::Write("vehicle: proxy created model=0x%08X seat=%d\n",
                      pv.vehicleModel, (int)pv.seat);
    }

    SetEntityCoords(s_playerVehicle, Vec3{pv.x, pv.y, pv.z});
    SetEntityHeading(s_playerVehicle, pv.heading);

    Init(natives::IS_PED_IN_VEHICLE);
    Push(s_ghost); Push(s_playerVehicle); Push(false);
    if (!Call())
    {
        Init(natives::SET_PED_INTO_VEHICLE);
        Push(s_ghost); Push(s_playerVehicle);
        Push(static_cast<unsigned long long>(
            static_cast<unsigned int>(static_cast<int>(pv.seat))));
        Call();
        sh_log::Write("vehicle: ghost seated at seat=%d\n", (int)pv.seat);
    }
    s_remoteInVehicle = true;
}

// P3: хост сравнивает каталог MissionData со своим baseline. Новое
// завершение/рейтинг отправляем гостю — он пишет зачёт в СВОЙ сейв.
void PollHostMissions()
{
    using namespace natives_api;
    int validCount = 0;
    int completedCount = 0;

    for (int i = 0; i < missions::kMissionCatalogSize; ++i)
    {
        const missions::MissionInfo& info = missions::kMissionCatalog[i];
        Init(natives::MISSIONDATA_IS_VALID);
        Push(static_cast<unsigned long long>(info.id));
        if (!Call())
        {
            if (s_missionBaseline.initialized)
            {
                s_missionBaseline.completed[i] = false;
                s_missionBaseline.rating[i] = 0;
            }
            continue;
        }
        ++validCount;

        Init(natives::MISSIONDATA_WAS_COMPLETED);
        Push(static_cast<unsigned long long>(info.id));
        const bool completed = Call() != 0;

        Init(natives::MISSIONDATA_GET_RATING);
        Push(static_cast<unsigned long long>(info.id));
        const int rating = static_cast<int>(Call());

        if (completed) ++completedCount;

        if (!s_missionBaseline.initialized)
        {
            s_missionBaseline.completed[i] = completed;
            s_missionBaseline.rating[i] = rating;
            continue;
        }

        if (completed && !s_missionBaseline.completed[i])
        {
            net::MissionEventPayload ev{};
            ev.missionId = info.id;
            ev.rating = static_cast<uint8_t>(rating < 0 ? 0 : (rating > 5 ? 5 : rating));
            ev.flags = net::kMissionFlagCompleted;
            sh_log::Write("mission: host completed 0x%08X (%s) rating=%d -> guest\n",
                          info.id, info.code, rating);
            g_netClient.SendMissionEvent(ev);
        }
        else if (s_missionBaseline.completed[i] && rating != s_missionBaseline.rating[i])
        {
            net::MissionEventPayload ev{};
            ev.missionId = info.id;
            ev.rating = static_cast<uint8_t>(rating < 0 ? 0 : (rating > 5 ? 5 : rating));
            ev.flags = net::kMissionFlagRatingUpdate;
            sh_log::Write("mission: host rating update 0x%08X (%s) %d -> %d\n",
                          info.id, info.code, s_missionBaseline.rating[i], rating);
            g_netClient.SendMissionEvent(ev);
        }

        s_missionBaseline.completed[i] = completed;
        s_missionBaseline.rating[i] = rating;
    }

    if (!s_missionBaseline.initialized)
    {
        s_missionBaseline.initialized = true;
        sh_log::Write("mission: baseline initialized (valid=%d completed=%d)\n",
                      validCount, completedCount);
    }
}

// P3: dry-run плана наград миссии. Нативы НЕ вызываются: только лог, чтобы
// сверить каталог и подготовить безопасное применение после тестов на двух ПК.
void LogMissionRewardPlan(uint32_t missionId)
{
    if (!coopcfg::Get().missionRewardLog)
        return;
    int count = 0;
    for (int i = 0; i < missions::kMissionRewardsSize; ++i)
    {
        const missions::MissionRewardPlan& r = missions::kMissionRewards[i];
        if (r.missionId != missionId)
            continue;
        const char* binding = "?";
        switch (r.binding)
        {
        case missions::RewardBinding::WeaponOwnership: binding = "weapon"; break;
        case missions::RewardBinding::UnlockVisible: binding = "unlock-visible"; break;
        case missions::RewardBinding::InventoryItem: binding = "inventory"; break;
        case missions::RewardBinding::RecipeUnlock: binding = "recipe"; break;
        case missions::RewardBinding::UnlockEntitlement: binding = "entitlement"; break;
        case missions::RewardBinding::WeaponShopEligibility: binding = "shop-eligibility"; break;
        }
        sh_log::Write("mission: reward plan 0x%08X %s record=0x%08X amount=%u "
                      "(dry-run, NOT applied)\n",
                      missionId, binding, r.recordHash, r.amount);
        ++count;
    }
    sh_log::Write("mission: reward plan dry-run done: %d record(s) for 0x%08X\n",
                  count, missionId);
}

// P3: гость — лог события всегда; зачёт в свой сейв только при mission_credit.
void ApplyMissionEventToGuest(const net::MissionEventPayload& ev)
{
    using namespace natives_api;
    const missions::MissionInfo* info = missions::FindMission(ev.missionId);
    const char* code = info ? info->code : "????";
    const char* title = info ? info->title : "unknown";

    if (!(ev.flags & (net::kMissionFlagCompleted | net::kMissionFlagRatingUpdate)))
        return;

    sh_log::Write("mission: host event 0x%08X (%s — %s) rating=%u flags=0x%02X\n",
                  ev.missionId, code, title, (unsigned)ev.rating, (unsigned)ev.flags);

    LogMissionRewardPlan(ev.missionId);

    if (!coopcfg::Get().missionCredit)
        return;

    if (!EnsureGuestSaveBackup())
        return;

    Init(natives::MISSIONDATA_IS_VALID);
    Push(static_cast<unsigned long long>(ev.missionId));
    if (!Call())
    {
        sh_log::Write("mission: credit skipped — MissionData invalid for this save\n");
        return;
    }

    Init(natives::MISSIONDATA_WAS_COMPLETED);
    Push(static_cast<unsigned long long>(ev.missionId));
    const bool already = Call() != 0;

    if (already)
    {
        // Уже зачтена: обновляем только рейтинг, completion повторно не пишем.
        Init(natives::_MISSIONDATA_SET_MISSION_RATING);
        Push(static_cast<unsigned long long>(ev.missionId));
        Push(static_cast<unsigned long long>(static_cast<unsigned int>(ev.rating)));
        Call();
        sh_log::Write("mission: already completed, rating updated to %u\n",
                      (unsigned)ev.rating);
        return;
    }

    Init(natives::_UILOG_MARK_MISSION_COMPLETED);
    Push(static_cast<unsigned long long>(ev.missionId));
    Call();

    Init(natives::_MISSIONDATA_SET_MISSION_RATING);
    Push(static_cast<unsigned long long>(ev.missionId));
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(ev.rating)));
    Call();

    sh_log::Write("mission: CREDITED to own save 0x%08X (%s) rating=%u "
                  "— save the game manually\n",
                  ev.missionId, code, (unsigned)ev.rating);
}

// Регистрация колбэков и запуск сетевого клиента.
// ВАЖНО: колбэки исполняются в фоновом сетевом потоке — они не имеют права
// трогать нативы/сущности движка; они только буферизуют состояние, которое
// Tick() (игровой поток) применит на следующем тике.
void InitNetworkClient()
{
    if (g_netStarted) return;
    g_netStarted = true;

    g_netClient.SetJoinOk([](const netio::JoinResult& jr) {
        sh_log::Write("net: JOIN_OK (hasSaved=%d, isHost=%d) from server\n",
                      jr.hasSaved ? 1 : 0, jr.isHost ? 1 : 0);
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.events.joinOk = true;
        s_inbox.events.joinIsHost = jr.isHost;
        s_inbox.events.joinHasHeading = jr.hasSaved;
        s_inbox.events.joinHeading = jr.hasSaved ? jr.heading : 0.f;
    });

    g_netClient.SetJoinRejected([](net::JoinRejectReason reason) {
        sh_log::Write("net: JOIN_REJECTED reason=%u "
                      "(1=version, 2=server full, 3=name taken, 4=bad name)\n",
                      static_cast<unsigned>(reason));
    });

    g_netClient.SetConnectionLost([]() {
        sh_log::Write("net: CONNECTION_LOST -> reconnect with backoff\n");
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.events.connectionLost = true;
    });

    g_netClient.SetHostChanged([](bool isHost) {
        sh_log::Write("net: HOST_CHANGED isHost=%d\n", isHost ? 1 : 0);
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.events.hostChanged = true;
        s_inbox.events.hostChangedIsHost = isHost;
    });

    // P1: WorldSnapshot от хоста. Сетевой поток только копирует payload;
    // прокси-сущности создаёт/обновляет world_sync из Tick() (игровой поток).
    g_netClient.SetWorldSnapshot([](const std::vector<uint8_t>& payload) {
        world_sync::OnSnapshot(payload);
    });

    // R&D: TrainSnapshot — привязка вагонов-прокси к локомотиву (игровой поток).
    g_netClient.SetTrainSnapshot([](const std::vector<uint8_t>& payload) {
        world_sync::OnTrainSnapshot(payload);
    });

    // P2: действие/сценарий удалённого игрока — только копируем; применит Tick().
    g_netClient.SetActionState([](const net::ActionStatePayload& a) {
        std::lock_guard<std::mutex> lk(s_actionMtx);
        s_actionIn = a;
        s_haveActionIn = true;
    });

    // P2: посадка удалённого игрока в транспорт — копируем; применит Tick().
    g_netClient.SetPlayerVehicle([](const net::PlayerVehicleState& pv) {
        std::lock_guard<std::mutex> lk(s_pvMtx);
        s_pvIn = pv;
        s_havePvIn = true;
    });

    // P3: MissionData-событие от хоста — буферизуем; применит Tick().
    g_netClient.SetMissionEvent([](const net::MissionEventPayload& ev) {
        std::lock_guard<std::mutex> lk(s_missionMtx);
        if (s_missionEvents.size() < 32)
            s_missionEvents.push_back(ev);
    });

    // P3: активная миссия хоста — буферизуем; применит Tick().
    g_netClient.SetMissionState([](const net::MissionStatePayload& st) {
        std::lock_guard<std::mutex> lk(s_missionStateMtx);
        s_missionStateIn = st;
        s_haveMissionState = true;
    });

    // P3: барьер миссии — буферизуем; применит Tick().
    g_netClient.SetMissionBarrier([](const net::MissionBarrierPayload& ev) {
        std::lock_guard<std::mutex> lk(s_barrierMtx);
        if (s_barrierEvents.size() < 32)
            s_barrierEvents.push_back(ev);
    });

    g_netClient.SetPlayerJoin([](const netio::PlayerJoinInfo& ji) {
        sh_log::Write("net: PLAYER_JOINED %s (model=0x%llX, pos=%d)\n",
                      ji.name.c_str(), ji.model, ji.hasPos ? 1 : 0);
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        InboxEvents& ev = s_inbox.events;
        ev.playerJoined = true;
        CopyInboxName(ev.playerJoinedName, ji.name);
        ev.playerJoinedModel = ji.model;
        ev.playerJoinedHasPos = ji.hasPos;
        if (ji.hasPos)
        {
            ev.playerJoinedX = ji.x;
            ev.playerJoinedY = ji.y;
            ev.playerJoinedZ = ji.z;
            ev.playerJoinedHeading = ji.heading;
        }
    });

    g_netClient.SetFrame([](const net::FramePayload& f) {
        // Только копируем кадр; нативы применит Tick().
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.frame = f;
        s_inbox.haveFrame = true;
    });

    g_netClient.SetWorldState([](const net::WorldState& ws) {
        // Только копируем; нативы (SET_CLOCK_TIME/SET_WEATHER_TYPE) вызовет
        // Tick() в игровом потоке.
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.worldState = ws;
        s_inbox.haveWorldState = true;
    });

    g_netClient.SetOutfit([](const net::OutfitState& os) {
        // Только копируем; нативы (EQUIP/UPDATE_VARIATION) вызовет Tick().
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.outfit = os;
        s_inbox.haveOutfit = true;
    });

    g_netClient.SetHorseOutfit([](const net::OutfitState& os) {
        // Сетевой поток: копируем; применит Tick().
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.horseOutfit = os;
        s_inbox.haveHorseOutfit = true;
    });

    g_netClient.SetShot([](const net::ShotPayload& sp) {
        // Сетевой поток: кладём в очередь, задачу стрельбы выдаст Tick().
        std::lock_guard<std::mutex> lk(s_shotMtx);
        if (s_shotQCount < kShotQueueSize)
        {
            s_shotQueue[s_shotQCount++] = Vec3{sp.x, sp.y, sp.z};
            s_shotEventsPending.store(true);
        }
        else
            s_shotDropped.fetch_add(1);
    });

    g_netClient.SetPlayerLeave([](const std::string& name) {
        sh_log::Write("net: PLAYER_LEFT %s\n", name.c_str());
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.events.leave = true;
        CopyInboxName(s_inbox.events.leaveName, name);
    });

    g_netClient.SetDied([](const std::string& name) {
        sh_log::Write("net: PLAYER_DIED %s\n", name.c_str());
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.events.died = true;
        CopyInboxName(s_inbox.events.diedName, name);
    });

    g_netClient.SetRespawned([](const std::string& name) {
        sh_log::Write("net: PLAYER_RESPAWNED %s\n", name.c_str());
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        s_inbox.events.respawned = true;
        CopyInboxName(s_inbox.events.respawnedName, name);
    });

    // Запуск с параметрами из rdr2-coop.ini (адрес/порт/ник/модель).
    coopcfg::Config& cfg = coopcfg::Load();
    sh_log::Write("net: connecting to %s:%d as '%s' (model=0x%llX)\n",
                  cfg.serverHost.c_str(), cfg.serverPort,
                  cfg.playerName.c_str(), cfg.pedModel);
    g_netClient.Start(cfg.serverHost, cfg.serverPort, cfg.playerName, cfg.pedModel);
}

} // namespace

// P1 (модель A): отправка WorldSnapshot от хоста. Вызывается только из
// world_sync::Tick() (игровой поток), сетевой клиент кладёт кадр в очередь.
void SendWorldSnapshot(const std::vector<uint8_t>& payload)
{
    g_netClient.SendWorldSnapshot(payload);
}

// R&D: TrainSnapshot от хоста. Гость пока только логирует.
void SendTrainSnapshot(const std::vector<uint8_t>& payload)
{
    g_netClient.SendTrainSnapshot(payload);
}

// P1: пед-призрак и его лошадь — не часть мира NPC; их нельзя реплицировать
// как обычные сущности (иначе на госте появятся дубли игрока).
bool IsWorldSyncExcluded(unsigned int handle)
{
    return handle != 0 && (handle == s_ghost || handle == s_mount);
}

void Tick()
{
    ++s_tickCount;
    // P1.1: hold выстрела уменьшаем каждый тик, а не только при приходе кадра
    // (иначе при паузе/лаге сети очередь выстрелов «замерзает»).
    if (s_shootHoldTicks > 0)
        --s_shootHoldTicks;
    if (s_deadGuardTicks > 0)
        --s_deadGuardTicks;

    unsigned int playerPed = GetPlayerPedFor(0);
    if (!playerPed)
        return;

    // Забрать из сетевого inbox последние кадр/наряд/состояние мира. Дальше
    // работаем только с игровыми копиями — гонок с сетевым потоком нет.
    {
        net::FramePayload frame{};
        if (TakeInboxFrame(frame))
        {
            s_lastFrame = frame;
            s_haveFrame = true;
            s_lastKnownPos = Vec3{frame.x, frame.y, frame.z};
            s_lastKnownHeading = frame.heading;
            s_lastKnownPosValid = true;
        }
        net::OutfitState os{};
        if (TakeInboxOutfit(os))
        {
            s_outfitIn = os;
            s_haveOutfit = true;
        }
        net::OutfitState ho{};
        if (TakeInboxHorseOutfit(ho))
        {
            s_horseOutfitIn = ho;
            s_haveHorseOutfit = true;
        }
        net::WorldState ws{};
        if (TakeInboxWorldState(ws))
        {
            s_worldState = ws;
            s_haveWorldState = true;
        }
    }

    // Spike 1 (модель A): диагностика мира — пулы сущностей, NPC/транспорт
    // рядом, mission-entity, FPS. Только чтение, геймплей не меняет.
    world_scan::Tick(playerPed, s_tickCount);

    // Start the network client on first tick (replaces the old offline
    // emulation as the source of join events).
    InitNetworkClient();

    // Действие игрока обновляем КАЖДЫЙ тик: короткие события (выстрел)
    // иначе теряются между отправками кадра.
    if (g_netStarted)
    {
        UpdateLocalActionState(playerPed);
        UpdateLocalAim(playerPed);
        UpdateLocalShotEvents(playerPed);

        // P1: репликация мира/NPC. Хост шлёт снапшоты, гость применяет их
        // и подавляет собственную ambient-популяцию. По умолчанию выключено
        // в ini (world_sync = 1 на ОБОИХ ПК для теста).
        world_sync::Tick(playerPed, s_tickCount, s_amHost.load(),
                         coopcfg::Get().worldSync,
                         coopcfg::Get().worldTrains);

        // P2: Action v2 — сценарии/сидение/курение/укрытие.
        if (coopcfg::Get().actionSync)
        {
            if ((s_tickCount % 3) == 0)
            {
                net::ActionStatePayload a = DetectLocalActionV2(playerPed);
                const bool changed = !s_lastSentActionValid ||
                    std::memcmp(&a, &s_lastSentAction, sizeof(a)) != 0;
                if (changed || s_tickCount - s_lastActionSendTick >= 15)
                {
                    s_lastSentAction = a;
                    s_lastSentActionValid = true;
                    s_lastActionSendTick = s_tickCount;
                    g_netClient.SendActionState(a);
                }
            }
            net::ActionStatePayload in{};
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(s_actionMtx);
                if (s_haveActionIn)
                {
                    in = s_actionIn;
                    s_haveActionIn = false;
                    have = true;
                }
            }
            if (have)
                ApplyRemoteActionV2(in);
        }

        // P2: посадка в транспорт удалённого игрока.
        if (coopcfg::Get().vehicleSync)
        {
            if ((s_tickCount % 3) == 0)
            {
                net::PlayerVehicleState pv = DetectLocalPlayerVehicle(playerPed);
                const bool changed = !s_lastSentPvValid ||
                    std::memcmp(&pv, &s_lastSentPv, sizeof(pv)) != 0;
                if (changed || s_tickCount - s_lastPvSendTick >= 15)
                {
                    s_lastSentPv = pv;
                    s_lastSentPvValid = true;
                    s_lastPvSendTick = s_tickCount;
                    g_netClient.SendPlayerVehicle(pv);
                }
            }
            net::PlayerVehicleState pv{};
            bool havePv = false;
            {
                std::lock_guard<std::mutex> lk(s_pvMtx);
                if (s_havePvIn)
                {
                    pv = s_pvIn;
                    s_havePvIn = false;
                    havePv = true;
                }
            }
            if (havePv)
                ApplyRemotePlayerVehicle(pv);
        }
        else
        {
            if (s_remoteInVehicle || s_playerVehicle)
                DeleteRemotePlayerVehicle();
            s_lastSentPvValid = false;
        }

        // P3: MissionData — хост детектит новые завершения, гость применяет.
        if (coopcfg::Get().missionSync)
        {
            if (s_amHost.load() && (s_tickCount - s_lastMissionPollTick) >= 60)
            {
                s_lastMissionPollTick = s_tickCount;
                PollHostMissions();
            }

            // Активная миссия хоста (runtime script) — для координированного
            // старта и подсказки гостю.
            if (s_amHost.load() && (s_tickCount - s_lastMissionStatePollTick) >= 30)
            {
                s_lastMissionStatePollTick = s_tickCount;
                const uint32_t activeId = DetectHostActiveMission();
                UpdateHostMissionBarrier(activeId);
                if (activeId != s_lastSentHostMissionId ||
                    (s_tickCount - s_lastMissionStateSendTick) >= 120)
                {
                    net::MissionStatePayload st{};
                    st.missionId = activeId;
                    if (activeId)
                    {
                        const missions::MissionInfo* info =
                            missions::FindMission(activeId);
                        st.runtimeHash = info ? missions::HashMissionId(info->runtime)
                                              : 0;
                        st.state = net::kMissionStateActive;
                        if (activeId != s_lastSentHostMissionId)
                        {
                            sh_log::Write("mission: host ACTIVE 0x%08X (%s — %s)\n",
                                          activeId, info ? info->code : "????",
                                          info ? info->title : "unknown");
                            s_joinedName[0] = '\0';
                        }
                    }
                    else
                    {
                        st.state = net::kMissionStateIdle;
                        if (s_lastSentHostMissionId)
                            sh_log::Write("mission: host active <- none\n");
                    }
                    s_lastSentHostMissionId = activeId;
                    s_lastMissionStateSendTick = s_tickCount;
                    g_netClient.SendMissionState(st);
                }
            }

            net::MissionStatePayload state{};
            bool haveState = false;
            {
                std::lock_guard<std::mutex> lk(s_missionStateMtx);
                if (s_haveMissionState)
                {
                    state = s_missionStateIn;
                    s_haveMissionState = false;
                    haveState = true;
                }
            }
            if (haveState)
            {
                if (state.state == net::kMissionStateActive && state.missionId)
                {
                    const missions::MissionInfo* info =
                        missions::FindMission(state.missionId);
                    sh_log::Write("mission: host is in 0x%08X (%s — %s)\n",
                                  state.missionId, info ? info->code : "????",
                                  info ? info->title : "unknown");
                    std::snprintf(s_joinedName, sizeof(s_joinedName),
                                  "Host mission: %s",
                                  info ? info->title : "unknown");
                    s_subtitleTicksLeft = 8 * 60;
                    s_remoteMissionId = state.missionId;
                }
                else if (s_remoteMissionId)
                {
                    sh_log::Write("mission: host mission ended\n");
                    s_remoteMissionId = 0;
                }
            }

            std::vector<net::MissionEventPayload> events;
            {
                std::lock_guard<std::mutex> lk(s_missionMtx);
                events.swap(s_missionEvents);
            }
            for (const net::MissionEventPayload& ev : events)
                ApplyMissionEventToGuest(ev);

            // P3: барьер совместного старта.
            std::vector<net::MissionBarrierPayload> barrierEvents;
            {
                std::lock_guard<std::mutex> lk(s_barrierMtx);
                barrierEvents.swap(s_barrierEvents);
            }
            for (const net::MissionBarrierPayload& ev : barrierEvents)
            {
                if (s_amHost.load())
                    ProcessHostMissionBarrier(ev);
                else
                    ProcessGuestMissionBarrier(ev);
            }

            if (!s_amHost.load() && s_guestBarrier.windowOpen &&
                !s_guestBarrier.startedSent &&
                (s_tickCount - s_lastGuestBarrierPollTick) >= 30)
            {
                s_lastGuestBarrierPollTick = s_tickCount;
                const uint32_t localActive = DetectHostActiveMission();
                if (localActive == s_guestBarrier.missionId)
                {
                    SendMissionBarrier(s_guestBarrier.missionId,
                                       s_guestBarrier.epoch,
                                       net::kMissionBarrierStarted, 0);
                    s_guestBarrier.startedSent = true;
                    s_guestBarrier.windowOpen = false;
                    const missions::MissionInfo* info =
                        missions::FindMission(s_guestBarrier.missionId);
                    sh_log::Write("barrier: guest STARTED own mission 0x%08X "
                                  "(%s) — window closed\n",
                                  s_guestBarrier.missionId,
                                  info ? info->code : "????");
                    std::snprintf(s_joinedName, sizeof(s_joinedName),
                                  "Mission started: %s",
                                  info ? info->title : "mission");
                    s_subtitleTicksLeft = 6 * 60;
                }
                else if (s_tickCount >= s_guestBarrier.deadlineTick)
                {
                    sh_log::Write("barrier: guest start window timeout\n");
                    s_guestBarrier.windowOpen = false;
                    std::snprintf(s_joinedName, sizeof(s_joinedName),
                                  "Mission start window expired");
                    s_subtitleTicksLeft = 6 * 60;
                }
            }
        }
        else
        {
            std::lock_guard<std::mutex> lk(s_missionMtx);
            s_missionEvents.clear();
            {
                std::lock_guard<std::mutex> lk2(s_barrierMtx);
                s_barrierEvents.clear();
            }
            s_hostBarrier = HostMissionBarrier{};
            s_guestBarrier = GuestMissionBarrier{};
        }
    }

    // Модель для педа: чужая (из PLAYER_JOINED), иначе дефолтная.
    // Если из кадра пришла модель персонажа — используем её (перезапись).
    // ВАЖНО: если кадр уже пришёл ДО спавна — сразу берём модель персонажа
    // из кадра, чтобы пед создавался с правильной моделью с первого раза
    // (иначе он создаётся горожанином и тут же пересоздаётся — анимации
    // рвутся, см. «remote model changed 0xCF7E73BE -> 0xD7114C9»).
    // Хэши моделей приходят из сети — валидируем IS_MODEL_VALID ДО RequestModel
    // и DeleteGhostEntity. Иначе битый хэш «залипал» в HasModelLoaded()==false,
    // Tick уходил в ранний return и переставал обрабатывать кадры/наряд/
    // выстрелы/connectionLost.
    if (s_requestedModel && !IsModelHashValid(s_requestedModel))
    {
        sh_log::Write("coop: ignoring invalid requested ped model 0x%llX\n",
                      s_requestedModel);
        s_requestedModel = 0;
    }
    if (s_remoteModel && !IsModelHashValid(s_remoteModel))
    {
        if (s_remoteModel != s_ignoredPedModelHash)
        {
            s_ignoredPedModelHash = s_remoteModel;
            sh_log::Write("coop: ignoring invalid remote ped model 0x%llX\n",
                          s_remoteModel);
        }
        s_remoteModel = 0;
    }
    if (s_haveFrame && s_lastFrame.reserved[2] && !s_requestedModel)
    {
        if (IsModelHashValid(s_lastFrame.reserved[2]))
        {
            s_requestedModel = s_lastFrame.reserved[2];
            RequestModel(s_requestedModel);
            s_modelLoadedLogged = false;
        }
        else if (s_lastFrame.reserved[2] != s_ignoredPedModelHash)
        {
            s_ignoredPedModelHash = s_lastFrame.reserved[2];
            sh_log::Write("coop: ignoring invalid frame ped model 0x%llX\n",
                          s_lastFrame.reserved[2]);
        }
    }
    unsigned long long wantModel = s_requestedModel ? s_requestedModel
                                  : (s_remoteModel ? s_remoteModel : GHOST_PED_MODEL);
    if (!IsModelHashValid(wantModel))
    {
        // Последний предохранитель: Tick не должен залипнуть в раннем return
        // навсегда. Берём модель локального игрока (валидна и уже загружена).
        using namespace natives_api;
        Init(natives::GET_ENTITY_MODEL);
        Push(playerPed);
        wantModel = Call();
        s_requestedModel = wantModel;
        RequestModel(wantModel);
        s_modelLoadedLogged = false;
        sh_log::Write("coop: invalid ped model -> fallback to local 0x%llX\n",
                      wantModel);
    }
    if (s_requestedModel != wantModel)
    {
        RequestModel(wantModel);
        s_requestedModel = wantModel;
        s_modelLoadedLogged = false;
    }

    if (!HasModelLoaded(wantModel))
    {
        if (!s_modelLoadedLogged)
        {
            sh_log::Write("coop: waiting for ped model 0x%llX to load\n", wantModel);
            s_modelLoadedLogged = true;
        }
        return;
    }

    // Забрать события из net-inbox (игровой поток) и превратить их в игровые
    // флаги/буферы. Раньше это делали сетевые колбэки напрямую — была гонка
    // с Tick и запись в общие char[] из чужого потока.
    InboxEvents ev;
    TakeInboxEvents(ev);

    if (ev.joinOk)
    {
        s_amHost.store(ev.joinIsHost);
        if (ev.joinHasHeading)
            s_pendingHeading = ev.joinHeading;
    }
    if (ev.hostChanged)
    {
        s_amHost.store(ev.hostChangedIsHost);
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      ev.hostChangedIsHost ? "You are now HOST" : "You are now GUEST");
        s_subtitleTicksLeft = 10 * 60;
    }
    // Порядок важен: сначала connectionLost, потом leave, потом playerJoined
    // (события нового сеанса должны перебить старые).
    if (ev.connectionLost)
    {
        s_pendingConnectionLost = true;
        // После reconnect наряд/снаряжение надо переслать заново.
        s_sentOutfitValid = false;
        s_sentHorseOutfitValid = false;
    }
    if (ev.leave)
    {
        std::snprintf(s_leaveName, sizeof(s_leaveName), "%s", ev.leaveName);
        s_pendingLeave = true;
    }
    if (ev.playerJoined)
    {
        sh_log::Write("coop: PLAYER_JOINED %s (model=0x%llX, pos=%d)\n",
                      ev.playerJoinedName, ev.playerJoinedModel,
                      ev.playerJoinedHasPos ? 1 : 0);
        std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s joined",
                      ev.playerJoinedName);
        s_playerJoinedNotified = true;
        s_pendingJoinWaitTicks = 0; // новый игрок — заново ждём его первый кадр
        s_subtitleTicksLeft = 8 * 60;
        std::snprintf(s_pedName, sizeof(s_pedName), "%s", ev.playerJoinedName);
        std::snprintf(s_pendingName, sizeof(s_pendingName), "%s", ev.playerJoinedName);
        // Модель из PLAYER_JOINED тоже приходит из сети — валидируем.
        if (ev.playerJoinedModel && !IsModelHashValid(ev.playerJoinedModel))
        {
            sh_log::Write("coop: ignoring invalid join model 0x%llX\n",
                          ev.playerJoinedModel);
            s_remoteModel = 0;
        }
        else
        {
            s_remoteModel = ev.playerJoinedModel;
        }
        if (ev.playerJoinedHasPos)
        {
            s_pendingHasPos = true;
            s_pendingX = ev.playerJoinedX;
            s_pendingY = ev.playerJoinedY;
            s_pendingZ = ev.playerJoinedZ;
            s_pendingHeading = ev.playerJoinedHeading;
        }
        else
        {
            s_pendingHasPos = false;
        }
        // Новому пиру наряд/снаряжение лошади надо переслать заново.
        s_sentOutfitValid = false;
        s_sentHorseOutfitValid = false;
    }

    // Обработка потери соединения: чистим удалённого педа и ждём reconnect.
    if (s_pendingConnectionLost)
    {
        s_pendingConnectionLost = false;
        s_pendingLeave = false;
        s_playerJoinedNotified = false;
        s_amHost.store(false);
        CleanupGhost();
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      "Connection lost — reconnecting...");
        s_subtitleTicksLeft = 10 * 60;
    }

    // Обработка выхода игрока (всё удаление — здесь, на игровом потоке).
    // Делаем ДО спавна, чтобы новый JOIN не перебился LEAVE от старого клиента.
    if (s_pendingLeave)
    {
        s_pendingLeave = false;
        // Надпись «Player X left» (как joined/dead). НЕ ставим
        // s_playerJoinedNotified = true — иначе этот же тик заспавнит
        // новый пед с именем по умолчанию («Player»). Для надписи
        // достаточно s_joinedName + s_subtitleTicksLeft.
        char leaveText[96];
        std::snprintf(leaveText, sizeof(leaveText), "Player %s left",
                      s_leaveName[0] ? s_leaveName : "Player");
        CleanupGhost();
        std::snprintf(s_joinedName, sizeof(s_joinedName), "%s", leaveText);
        s_subtitleTicksLeft = 8 * 60;
        s_leaveName[0] = '\0';
    }

    // Смерть удалённого игрока от СЕРВЕРА (оригинал умер в своём мире):
    // снимаем invincibility, убиваем педа (труп и снаряжение остаются) и
    // сбрасываем боевое состояние — иначе после респавна выстрелы «висят» на
    // старом оружии/очереди. Обрабатываем ДО кадра, чтобы труп не двигался.
    if (ev.died)
    {
        const char* who = ev.diedName[0] ? ev.diedName : "Player";
        if (s_ghost && DoesEntityExist(s_ghost) && !s_ghostDead)
        {
            using namespace natives_api;
            Init(natives::SET_ENTITY_INVINCIBLE);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(0));
            Call();
            Init(natives::SET_ENTITY_HEALTH);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(0)); // 0 HP = смерть
            Push(static_cast<unsigned long long>(0)); // entityKilledBy
            Call();
            s_ghostDead = true;
            s_deadGuardTicks = 120; // ~2 с: не воскрешать кадром health>0.2
            ResetGhostCombatState();
            sh_log::Write("coop: [DEAD] %s (killed by server Died)\n", who);
            if (s_blip)
                s_blip = RemoveBlip(s_blip);
            std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s is dead", who);
            s_subtitleTicksLeft = 8 * 60;
            s_pedName[0] = '\0'; // имя над головой скрыто
        }
        else
        {
            sh_log::Write("coop: Died for %s but no ghost\n", who);
        }
    }

    // Оригинал ожил после смерти — возрождаем педа. Свежий кадр этого тика
    // (если уже пришёл) даёт актуальную позицию; иначе RespawnGhostPed берёт
    // s_lastKnownPos, а не позицию входа в сессию.
    if (ev.respawned)
    {
        std::snprintf(s_remoteRespawnName, sizeof(s_remoteRespawnName), "%s",
                      ev.respawnedName);
        RespawnGhostPed(s_haveFrame ? &s_lastFrame : nullptr);
    }

    // Спавн педа: только при PLAYER_JOINED (не при своём JOIN_OK).
    // ВАЖНО: не спавним сразу с «подставной» моделью из JOIN (горожанин) —
    // через миг прилетит кадр с реальной моделью персонажа (reserved[2]),
    // и пед придётся пересоздавать (Т-поза + слёт одежды). Ждём первый кадр
    // до ~2 секунд: если пришёл — спавним сразу с моделью персонажа.
    if (!s_ghost && s_playerJoinedNotified)
    {
        // Если кадр с моделью персонажа ещё не получен — ждём (но не вечно).
        if (!s_haveFrame || !s_lastFrame.reserved[2])
        {
            unsigned int waitTicks = s_pendingJoinWaitTicks++;
            if (waitTicks < 2 * 60) // до ~2 секунд
                return;
        }
    }
    if (!s_ghost && s_playerJoinedNotified)
    {
        Vec3 spawn;
        float heading = s_pendingHeading;
        const char* name = s_pendingName[0] ? s_pendingName : "Player";
        if (s_pendingHasPos)
        {
            // Сервер прислал сохранённую/последнюю позицию игрока — спавним там.
            spawn = Vec3{s_pendingX, s_pendingY, s_pendingZ};
            sh_log::Write("coop: [SPAWN] %s at saved pos (%.1f %.1f %.1f)\n",
                          name, spawn.x, spawn.y, spawn.z);
        }
        else
        {
            // Новый игрок без сохранённой позиции — спавним рядом с хостом.
            Vec3 pc = GetEntityCoords(playerPed);
            spawn = pc;
            spawn.x += SPAWN_DISTANCE;
            spawn.z += 0.5f;
            sh_log::Write("coop: [SPAWN] %s next to host (first visit)\n", name);
        }
        // Вычислить высоту земли ДО создания педа: создать педа на z кадра
        // («пояс» игрока, может быть высоко) нельзя — пед успеет упасть с
        // высоты до того, как мы поставим его на землю, и разобьётся
        // (наблюдали: телепорт в небо, падение, смерть педа на ПК2).
        float groundZ = 0.f;
        const bool hasGround = GetGroundZForCoord(spawn, groundZ);
        Vec3 createAt = spawn;
        if (hasGround)
            createAt.z = groundZ + 0.2f; // чуть над землёй
        s_ghost = CreateGhostPed(wantModel, createAt, heading);
        if (s_ghost)
            s_ghostModel = wantModel; // фиксируем модель созданного педа
        sh_log::Write(s_ghost ? "coop: ghost ped created (on join), handle=%u at (%.1f %.1f %.1f) ground=%d\n"
                              : "coop: CREATE_PED returned 0\n",
                      s_ghost, createAt.x, createAt.y, createAt.z, hasGround ? 1 : 0);
        if (s_ghost)
        {
            SetEntityMission(s_ghost);
            s_appliedOutfitValid = false; // новый пед — наряд наденем заново
            ResetGhostCombatState();      // новому педу оружие выдадим заново
            s_deadGuardTicks = 0;
            // ВАЖНО: RandomizePedComponents (SET_PED_RANDOM_COMPONENT_VARIATION)
            // ОБЯЗАТЕЛЕН для рендера модели педа — без него пед невидим
            // (остаются только прикреплённые shop-предметы-одёжки). Делаем это
            // ОДИН раз на педа; дальше наряд применяется поверх.
            RandomizePedComponents(s_ghost);
            s_ghostBaseRandomized = true;
            SetEntityVisibleForce(s_ghost);
            PlaceOnGround(s_ghost);
            if (hasGround)
                SetEntityCoords(s_ghost, Vec3{spawn.x, spawn.y, groundZ + 0.2f});
            TaskStandStill(s_ghost);
            SetGhostStaminaFull(); // не устаёт и не «отдувается» после бега
            s_blip = CreateFriendlyBlipAt(spawn);
            sh_log::Write("coop: friendly blip -> %u\n", s_blip);
            std::snprintf(s_pedName, sizeof(s_pedName), "%s", name);
            // Наряд мог прийти раньше создания педа (не съедаем его в Tick) —
            // применяем сразу, иначе без следующей смены одежды он потеряется.
            if (s_outfitIn.count)
                ApplyOutfitToGhost(s_outfitIn);
        }
        // Применить самый свежий кадр, если уже пришёл (иначе пед «догонит»
        // позицию на следующем кадре).
        if (s_haveFrame)
        {
            ApplyRemoteFrame(s_lastFrame);
            s_haveFrame = false;
        }
    }

    // Применить входящий кадр удалённого игрока (игровой поток).
    if (s_haveFrame)
    {
        CheckRemoteModelChange(s_lastFrame);
        // Точка прицела удалённого игрока — для наведения педа.
        s_remoteAimValid = (s_lastFrame.aimDist > 1.0f);
        if (s_remoteAimValid)
            s_remoteAim = Vec3{s_lastFrame.aimX, s_lastFrame.aimY, s_lastFrame.aimZ};
        ApplyRemoteFrame(s_lastFrame);
        s_haveFrame = false;
    }

    // Событие выстрела: разбираем очередь по одному. Это НЕЗАВИСИМЫЙ от
    // action-hold таймер: иначе кадры Action::Shooting постоянно продлевают
    // hold=30 и очередь не успевает проигрываться. Интервал 5 тиков ~=
    // 12 выстрелов/с — покрывает автоматическое оружие (10/с) целиком,
    // очередь на 64 события страхует длинные серии.
    if (s_shotDropped.load() > 0 && (s_tickCount % 300) == 1)
    {
        sh_log::Write("coop: shot queue overflow, dropped=%d\n",
                      s_shotDropped.load());
        s_shotDropped.store(0);
    }
    int shotPending = 0;
    {
        std::lock_guard<std::mutex> lk(s_shotMtx);
        shotPending = s_shotQCount;
    }
    if (shotPending > 0 && s_ghost && DoesEntityExist(s_ghost) && !s_ghostDead &&
        s_tickCount - s_lastShotEventTaskTick >= 5)
    {
        Vec3 t{0.f, 0.f, 0.f};
        {
            std::lock_guard<std::mutex> lk(s_shotMtx);
            if (s_shotQCount > 0)
            {
                t = s_shotQueue[0];
                for (int i = 1; i < s_shotQCount; ++i)
                    s_shotQueue[i - 1] = s_shotQueue[i];
                --s_shotQCount;
                if (s_shotQCount == 0)
                    s_shotEventsPending.store(false);
            }
        }
        if (t.x == 0.f && t.y == 0.f && t.z == 0.f)
            t = GhostAimPointAhead(); // нет данных — вперёд
        ShootGhostAt(t, 100);
        s_lastShotEventTaskTick = s_tickCount;
        // Небольшой hold, чтобы прицел не перебил анимацию выстрела; очередь
        // его НЕ ждёт (см. условие выше).
        s_shootHoldTicks = 10;
        s_combatActive = true;
        s_lastAction = static_cast<uint8_t>(net::Action::Shooting);
        sh_log::Write("coop: shot event -> (%.1f %.1f %.1f)\n", t.x, t.y, t.z);
    }

    // Применить компоненты одежды от удалённого игрока (при смене наряда).
    // Если педа ещё нет — НЕ съедаем флаг: применим сразу после его создания
    // (иначе первое сообщение об одежде терялось и призрак оставался рандомным).
    if (s_haveOutfit && s_ghost && DoesEntityExist(s_ghost) && !s_ghostDead)
    {
        s_haveOutfit = false;
        ApplyOutfitToGhost(s_outfitIn);
    }
    // Снаряжение лошади удалённого игрока — применить к лошади-призраку.
    if (s_haveHorseOutfit && s_mount && DoesEntityExist(s_mount))
    {
        s_haveHorseOutfit = false;
        ApplyHorseOutfitToMount(s_horseOutfitIn);
    }
    // Применить отложенный наряд, если модель педа наконец совпала.
    TryApplyPendingOutfit();

    // «Догоняющее» применение наряда после посадки/спешивания: движок в эти
    // моменты несколько раз сбрасывает component variation, и один наш
    // apply может быть перебит. Базу НЕ рандомизируем повторно — только
    // переприменяем наряд (случайные слои больше не подмешиваются).
    if (s_outfitRefreshTicks > 0)
    {
        --s_outfitRefreshTicks;
        if (s_ghost && DoesEntityExist(s_ghost) && !s_ghostDead &&
            s_outfitIn.count && s_tickCount % 20 == 0)
        {
            RefreshGhostComponents();
        }
    }

    // Draw the remote player's name over the ped's head while within range.
    if (s_ghost && s_pedName[0] && DoesEntityExist(s_ghost))
    {
        Vec3 pc = GetEntityCoords(playerPed);
        Vec3 gp = GetEntityCoords(s_ghost);
        float dist = GetDistanceBetweenCoords(pc, gp);
        if (dist <= NAME_VISIBILITY_RANGE)
        {
            // Place the name above the head: take the ped's feet coords and
            // add a fixed head-height offset (GET_PED_BONE_COORDS for the
            // head returns mid-body in this build, so we use a constant).
            Vec3 aboveHead = gp;
            aboveHead.z += 1.4f; // ~0.5 м над макушкой (опущено на 25% от 1.9f)
            DrawNameAt(aboveHead, s_pedName);
        }
    }

    // Keep the ped's blip on its current position (coords-based blip).
    if (s_blip && s_ghost && DoesEntityExist(s_ghost))
    {
        Vec3 gp = GetEntityCoords(s_ghost);
        UpdateBlipCoords(s_blip, gp);
    }

    // Смерть удалённого игрока: метка-крестик, скрыть имя и показать
    // короткое оповещение (однократно). Грация после возрождения — не
    // считаем педа мёртвым, пока RESURRECT_PED не применился.
    if (s_reviveGraceTicks > 0)
        --s_reviveGraceTicks;
    if (s_reviveGraceTicks == 0 && s_ghost && DoesEntityExist(s_ghost)
        && IsPedDeadOrDying(s_ghost) && !s_ghostDead)
    {
        s_ghostDead = true;
        // Пед призрака invincible; чтобы смерть действительно проигралась,
        // снимаем флаг, затем (как и по серверному Died) сбрасываем бой.
        using namespace natives_api;
        Init(natives::SET_ENTITY_INVINCIBLE);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(0));
        Call();
        ResetGhostCombatState();
        sh_log::Write("coop: [DEAD] %s\n", s_pedName[0] ? s_pedName : "Player");
        if (s_blip)
            s_blip = RemoveBlip(s_blip);
        // короткое оповещение, как при подключении
        std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s is dead",
                      s_pedName[0] ? s_pedName : "Player");
        s_subtitleTicksLeft = 8 * 60;
        s_pedName[0] = '\0'; // имя над головой скрыто
    }

    // Смерть/возрождение локального игрока — уведомляем сервер однократно.
    if (g_netStarted && DoesEntityExist(playerPed))
    {
        if (!s_localDiedNotified && IsPedDeadOrDying(playerPed))
        {
            s_localDiedNotified = true;
            g_netClient.SendDied();
            sh_log::Write("coop: local player died, sent Died\n");
        }
        else if (s_localDiedNotified && !IsPedDeadOrDying(playerPed))
        {
            // Ожили — сбрасываем флаг, чтобы следующая смерть снова уведомила,
            // и сообщаем серверу «ожил» (партнёр возродит педа-призрака).
            s_localDiedNotified = false;
            g_netClient.SendRespawned();
            sh_log::Write("coop: local player revived, sent Respawned\n");
        }
    }

    // Persist the ped's position regularly so a returning player spawns on
    // their last known coords. Write at a low frequency to avoid hammering
    // the file (every ~5s at 60 ticks/s).
    if (s_ghost && s_pedName[0] && DoesEntityExist(s_ghost) &&
        (s_tickCount % (5 * 60) == 1))
    {
        Vec3 gp = GetEntityCoords(s_ghost);
        float hd = GetEntityHeading(s_ghost);
        persist::SaveLastPosition(s_pedName, gp.x, gp.y, gp.z, hd);
    }

    // Отправка собственного GhostFrame на сервер (каждые ~4 тика, чтобы
    // сервер видел координаты игрока в таблице состояния и ретранслировал
    // их другим игрокам). В кадре передаём move/state/flags/health игрока.
    if (g_netStarted)
    {
        Vec3 pc = GetEntityCoords(playerPed);
        net::FramePayload fp{};
        fp.x = pc.x; fp.y = pc.y; fp.z = pc.z;
        fp.heading = GetEntityHeading(playerPed);
        fp.move = DetectLocalMoveState(playerPed);
        fp.state = DetectLocalPedState(playerPed);
        fp.flags = DetectLocalFlags(playerPed);
        fp.action = s_localActionStable;
        fp.health = GetLocalHealth01(playerPed);
        // Точка прицела (куда целится/стреляет игрок) — для точной наводки.
        if (s_localAimValid)
        {
            fp.aimX = s_localAimX;
            fp.aimY = s_localAimY;
            fp.aimZ = s_localAimZ;
            float dx = fp.aimX - fp.x, dy = fp.aimY - fp.y, dz = fp.aimZ - fp.z;
            fp.aimDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        fp.weaponHash = GetLocalWeaponHash(playerPed);
        // Мета-наряд игрока (для синхронизации внешнего вида у партнёра).
        {
            using namespace natives_api;
            Init(natives::GET_PED_META_OUTFIT_HASH);
            Push(playerPed);
            fp.reserved[1] = Call();
            // Модель персонажа (Артур/Джон) — чтобы пед был копией, а не
            // подставной моделью из конфига.
            Init(natives::GET_ENTITY_MODEL);
            Push(playerPed);
            fp.reserved[2] = Call();
            // Диагностика: что реально уходит в кадре (редко).
            if (s_tickCount % (5 * 60) == 1)
                sh_log::Write("coop: send pedLook, outfit=0x%llX model=0x%llX "
                              "move=%u state=%u\n",
                              fp.reserved[1], fp.reserved[2], fp.move, fp.state);
        }
        // Если игрок верхом — передаём модель его лошади и её снаряжение.
        if (fp.move == static_cast<uint8_t>(coop::MoveState::RideHorse))
        {
            using namespace natives_api;
            Init(natives::GET_MOUNT); Push(playerPed);
            unsigned int m = static_cast<unsigned int>(Call());
            if (m)
            {
                Init(natives::GET_ENTITY_MODEL); Push(m);
                fp.reserved[0] = Call();
                // Снаряжение: shop-предметы лошади (мета-наряд у лошадей
                // всегда 0, поэтому собираем компоненты как у педа).
                net::OutfitState ho{};
                CollectLocalOutfit(m, ho);
                // Сравнение order-insensitive + heartbeat (после реконнекта
                // получатель мог не получить снаряжение вовсе).
                const bool changed = !s_sentHorseOutfitValid ||
                                     !OutfitsEqual(ho, s_sentHorseOutfit);
                const bool heartbeat =
                    (s_tickCount - s_lastHorseOutfitSendTick) >= kOutfitHeartbeatTicks;
                if (ho.count > 0 && (changed || heartbeat))
                {
                    s_sentHorseOutfit = ho;
                    s_sentHorseOutfitValid = true;
                    s_lastHorseOutfitSendTick = s_tickCount;
                    g_netClient.SendHorseOutfit(ho);
                    sh_log::Write("coop: send horse outfit (%u comp)%s\n",
                                  ho.count, changed ? "" : " [heartbeat]");
                }
            }
            else if (s_tickCount % (3 * 60) == 1)
            {
                sh_log::Write("coop: send RideHorse but GET_MOUNT=0\n");
            }
        }
        fp.seq = s_tickCount;
        g_netClient.SendFrame(fp);
    }

    // Синхронизация одежды: собираем компоненты наряда локального игрока и
    // отправляем при их смене (не каждый кадр). У сюжетных персонажей
    // GET_PED_META_OUTFIT_HASH = 0, поэтому используем список компонентов.
    // Смена отправляется только если набор продержался ~0.5 с: доставание
    // оружия (кобуры/патронташи) дёргает список и вызывает лишние применения.
    // Сравнение — order-insensitive (порядок категорий от движка нестабилен),
    // плюс heartbeat раз в ~5 с: иначе после реконнекта одежда не пересылается.
    if (g_netStarted)
    {
        net::OutfitState os{};
        CollectLocalOutfit(playerPed, os);
        const bool changed = !s_sentOutfitValid || !OutfitsEqual(os, s_sentOutfit);
        bool sendNow = false;
        bool heartbeat = false;
        if (os.count > 0 && changed)
        {
            if (s_haveCandidateOutfit && OutfitsEqual(os, s_candidateOutfit))
            {
                ++s_candidateOutfitTicks;
            }
            else
            {
                s_candidateOutfit = os;
                s_haveCandidateOutfit = true;
                s_candidateOutfitTicks = 1;
            }
            if (s_candidateOutfitTicks >= kOutfitDebounceTicks &&
                s_tickCount - s_lastOutfitSendTick >= kOutfitMinIntervalTicks)
                sendNow = true;
        }
        else if (os.count > 0 &&
                 (s_tickCount - s_lastOutfitSendTick) >= kOutfitHeartbeatTicks)
        {
            sendNow = true;
            heartbeat = true;
        }
        if (sendNow)
        {
            // Диагностика: какие позиции изменились (чтобы понять, что
            // именно дёргает оружие — кобура/патронташ/пояс).
            int diffCount = 0, firstIdx = -1;
            unsigned int oldV = 0, newV = 0;
            for (int i = 0; i < net::kMaxOutfitComponents; ++i)
            {
                if (os.components[i] != s_sentOutfit.components[i])
                {
                    ++diffCount;
                    if (firstIdx < 0)
                    {
                        firstIdx = i;
                        oldV = s_sentOutfit.components[i];
                        newV = os.components[i];
                    }
                }
            }
            s_lastOutfitSendTick = s_tickCount;
            s_haveCandidateOutfit = false;
            s_candidateOutfitTicks = 0;
            s_sentOutfit = os;
            s_sentOutfitValid = true;
            g_netClient.SendOutfit(os);
            char comps[256] = "";
            {
                int off = 0;
                for (uint32_t i = 0; i < os.count && off < 220; ++i)
                    off += std::snprintf(comps + off, sizeof(comps) - off,
                                         "%s0x%X", i ? " " : "", os.components[i]);
            }
            sh_log::Write("coop: send outfit (%u comp, model 0x%llX, diff=%d "
                          "first[%d]=0x%X->0x%X)%s [%s]\n",
                          os.count, os.model, diffCount, firstIdx, oldV, newV,
                          heartbeat ? " [heartbeat]" : "", comps);
        }
    }

    // Синхронизация мира (время + погода): хост шлёт своё состояние каждые
    // ~5 секунд, остальные применяют его у себя (SET_CLOCK_TIME / погода).
    if (g_netStarted && s_amHost.load())
    {
        // Раз в 5 секунд (300 тиков) отправляем текущее время/погоду.
        if (s_tickCount - s_lastWorldSendTick >= 5 * 60)
        {
            s_lastWorldSendTick = s_tickCount;
            net::WorldState ws{};
            using namespace natives_api;
            Init(natives::GET_CLOCK_HOURS);   ws.hour   = static_cast<uint8_t>(Call());
            Init(natives::GET_CLOCK_MINUTES); ws.minute = static_cast<uint8_t>(Call());
            Init(natives::GET_CLOCK_SECONDS); ws.second = static_cast<uint8_t>(Call());
            // Текущее состояние погоды: два типа + доля перехода.
            uint32_t w1 = 0, w2 = 0; float pct = 0.f;
            void* a = nullptr, * b = nullptr;
            Init(natives::GET_CURR_WEATHER_STATE);
            Push(reinterpret_cast<unsigned long long>(&w1));
            Push(reinterpret_cast<unsigned long long>(&w2));
            Push(reinterpret_cast<unsigned long long>(&pct));
            Call();
            ws.weatherType1 = w1;
            ws.weatherType2 = w2;
            ws.weatherPct = pct;
            g_netClient.SendWorldState(ws);
            if (s_tickCount % (15 * 60) == 1) // редкий лог
                sh_log::Write("coop: host world state %02u:%02u:%02u w=%08X/%08X %.2f\n",
                              ws.hour, ws.minute, ws.second,
                              ws.weatherType1, ws.weatherType2, ws.weatherPct);
        }
    }
    else if (s_haveWorldState)
    {
        // Не хост: применяем время и погоду хоста.
        s_haveWorldState = false;
        const net::WorldState& ws = s_worldState;
        using namespace natives_api;
        // Применяем часы только при заметном расхождении (иначе дёргаем
        // секунды каждый кадр — секунды всё равно идут сами).
        int ch = 0, cm = 0;
        Init(natives::GET_CLOCK_HOURS);   ch = static_cast<int>(Call());
        Init(natives::GET_CLOCK_MINUTES); cm = static_cast<int>(Call());
        if (ch != ws.hour || cm != ws.minute || s_lastAppliedHour < 0)
        {
            Init(natives::SET_CLOCK_TIME);
            Push(static_cast<unsigned long long>(ws.hour));
            Push(static_cast<unsigned long long>(ws.minute));
            Push(static_cast<unsigned long long>(ws.second));
            Call();
            s_lastAppliedHour = ws.hour;
            s_lastAppliedMin = ws.minute;
            sh_log::Write("coop: apply host time %02u:%02u:%02u\n",
                          ws.hour, ws.minute, ws.second);
        }
        // Погода: SET_WEATHER_TYPE переключает сразу (с плавным переходом).
        if (ws.weatherType1 || ws.weatherType2)
        {
            Init(natives::SET_WEATHER_TYPE);
            Push(static_cast<unsigned long long>(ws.weatherType2 ? ws.weatherType2
                                                                 : ws.weatherType1));
            Push(static_cast<unsigned long long>(1));  // p1
            Push(static_cast<unsigned long long>(0));  // p2
            Push(static_cast<unsigned long long>(1));  // transition
            Push(0.5f);                                // transitionTime (сек)
            Push(static_cast<unsigned long long>(0));  // p5
            Call();
        }
    }

    // Small "Player X joined" subtitle on screen while a join happened
    // (drawn each frame for a short while proves the text path works).
    // Гейт по тексту/таймеру, а не по s_playerJoinedNotified: этот флаг теперь
    // означает только «удалённый игрок в сессии» (гейт спавна), а субтитры
    // показываем и для death/respawn/host-changed/connection-lost.
    if (s_joinedName[0])
    {
        if (s_subtitleTicksLeft > 0)
        {
            if (s_subtitleTicksLeft == 8 * 60) // первый тик нового таймера — лог для диагностики
                sh_log::Write("coop: subtitle: '%s' (%u ticks)\n", s_joinedName, s_subtitleTicksLeft);
            DrawSubtitle(s_joinedName);
            --s_subtitleTicksLeft;
        }
        else
        {
            s_joinedName[0] = '\0'; // hide permanently until next event
        }
    }

    if (s_tickCount % 120 == 1 && s_ghost && DoesEntityExist(s_ghost))
        sh_log::Write("coop: ghost ped alive, handle=%u\n", s_ghost);

    // Задержка до сервера: окно мин/макс обновляется раз в 2 с, но строка
    // РИСУЕТСЯ КАЖДЫЙ КАДР — _BG_DISPLAY_TEXT живёт один кадр, иначе надпись
    // мелькает миллисекунду и пропадает.
    if (g_netStarted)
    {
        uint32_t rtt = g_netClient.RttMs();
        if (rtt > 0)
        {
            if (s_rttMin == 0 || rtt < s_rttMin) s_rttMin = rtt;
            if (rtt > s_rttMax) s_rttMax = rtt;
        }
        if (s_tickCount - s_lastRttDrawTick >= 2 * 60)
        {
            s_lastRttDrawTick = s_tickCount;
            // Джиттер: |rtt - prev| со сглаживанием (RFC 3550-стиль).
            if (s_rttPrev)
            {
                float diff = rtt > s_rttPrev ? (float)(rtt - s_rttPrev)
                                             : (float)(s_rttPrev - rtt);
                s_rttJitter = s_rttJitter * 0.75f + diff * 0.25f;
            }
            s_rttPrev = rtt;
            std::snprintf(s_rttHud, sizeof(s_rttHud),
                          "RTT %u ms jit %.0f (min %u / max %u)",
                          rtt, s_rttJitter, s_rttMin, s_rttMax);
            sh_log::Write("net: rtt %u ms (min %u / max %u, jit %.0f)\n",
                          rtt, s_rttMin, s_rttMax, s_rttJitter);
            s_rttMin = rtt;
            s_rttMax = rtt;
        }
        // _BG_DISPLAY_TEXT рисует строку только на ОДИН кадр, поэтому
        // вызываем каждый тик с уже сформированным текстом (иначе моргает).
        if (s_rttHud[0])
            DrawHudLine(s_rttHud);
    }
}

} // namespace coop
