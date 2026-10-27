#include "coop.h"
#include "config.h"
#include "ghost_frame.h"
#include "log.h"
#include "natives.h"
#include "script_hook.h"
#include "net/client.h"
#include "persist.h"
#include "world_scan.h"
#include "world_sync.h"
#include "actor_sync.h"
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

// P0-1 (task-156): полный замер кадра (QPC), p95/max по окну ~5 c.
unsigned long long QpcNow()
{
    static long long freq = 0;
    if (!freq)
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<unsigned long long>(
        static_cast<double>(c.QuadPart) * 1000000.0 / freq);
}
unsigned long long QpcDeltaUs(unsigned long long start)
{
    const unsigned long long now = QpcNow();
    return now > start ? now - start : 0;
}
struct PerfRing
{
    double v[512] = {};
    int pos = 0;
    int count = 0;
    void Add(double us)
    {
        v[pos] = us;
        pos = (pos + 1) % 512;
        if (count < 512) ++count;
    }
    double Avg() const
    {
        if (!count) return 0.0;
        double sum = 0.0;
        for (int i = 0; i < count; ++i) sum += v[i];
        return sum / count;
    }
    double P95()
    {
        if (!count) return 0.0;
        double tmp[512];
        for (int i = 0; i < count; ++i) tmp[i] = v[i];
        const int idx = (count * 95) / 100;
        std::nth_element(tmp, tmp + idx, tmp + count);
        return tmp[idx];
    }
    double Max() const
    {
        double m = 0.0;
        for (int i = 0; i < count; ++i) m = std::max(m, v[i]);
        return m;
    }
    void Reset() { pos = 0; count = 0; }
};
PerfRing g_framePerf, g_worldPerf, g_actorPerf, g_coopPerf;
unsigned int g_perfWindowTicks = 0;
unsigned int g_frameSentWindow = 0;

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

    // E6 (v18): UI-статус удалённого игрока (карта/пауза/сатчель/магазин).
    bool presenceValid = false;
    uint32_t presenceEpoch = 0;
    uint16_t presenceFlags = 0;
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

// ---------------- E6 (v18): UI-статус (presence) ----------------
// RAGE-хэши имён UI-приложений (joaat lowercase, из задачи E6).
constexpr unsigned long long kUiAppPauseMenu = 0x01C46AD2ull;
constexpr unsigned long long kUiAppMap       = 0x8646B488ull;
constexpr unsigned long long kUiAppSatchel   = 0xFFC21415ull;
constexpr unsigned long long kUiAppShop      = 0xDBE4BF79ull;
constexpr unsigned long long kUiAppPlayerMenu = 0x556A13C4ull;
constexpr unsigned long long kUiAppInventory = 0x3008C430ull;

constexpr unsigned int kPresenceDetectPeriodTicks = 5;
constexpr unsigned int kPresenceHeartbeatTicks = 120; // ~2 c
constexpr unsigned int kPresenceMaxPerSecond = 10;
constexpr unsigned int kPresenceLogEveryTicks = 60;   // 1 строка/с
// Удалённый статус без обновлений дольше ~12 с считаем устаревшим (heartbeat
// 2 с; запас на паузу/лаг, но без «вечного» суффикса после пропажи партнёра).
constexpr unsigned int kPresenceRemoteTimeoutTicks = 12 * 60;

uint32_t s_presenceEpoch = 1;             // локальная эпоха сессии (<> 0)
uint16_t s_localPresenceFlags = 0;
uint16_t s_lastSentPresenceFlags = 0;
bool s_presenceSentValid = false;
unsigned int s_lastPresenceDetectTick = 0;
unsigned int s_lastPresenceSendTick = 0;
unsigned long long s_lastPresenceSendMs = 0;
unsigned int s_presenceWindowTick = 0;
unsigned int s_presenceWindowCount = 0;
unsigned long long s_presenceWindowMs = 0;
unsigned int s_lastUiPresenceLogTick = 0;
bool s_remotePresenceValid = false;
uint16_t s_remotePresenceFlags = 0;
unsigned int s_lastRemotePresenceTick = 0;
// E7.1: visitor mode (гость подавляет локальную ambient-популяцию).
bool s_joinOkSeen = false;
bool s_visitorModeActive = false;
// E7.1 v2: cleanup существующей ambient-толпы (гость) + road population.
constexpr unsigned int kVisitorCleanupIntervalTicks = 600; // ~10 c
constexpr float kVisitorCleanupRadius = 120.0f;
constexpr int kVisitorCleanupQueueMax = 32;
constexpr unsigned int kVisitorCleanupFadeTicks = 24; // ~0.4 c
constexpr int kVisitorCleanupDeletePerTick = 8;
constexpr unsigned int kClearAreaAmbientFlag = 1048576u;
struct VisitorCleanupPed
{
    unsigned int handle = 0;
    unsigned int fade = 0;
};
VisitorCleanupPed s_visitorCleanup[kVisitorCleanupQueueMax];
int s_visitorCleanupCount = 0;
unsigned int s_lastVisitorCleanupTick = 0;
unsigned int s_visitorCleanupRemoved = 0;
unsigned int s_lastVisitorCleanupLogTick = 0;
bool s_visitorCleanupRanOnce = false;
bool s_visitorRoadPopDisabled = false;
bool s_visitorScenePaused = false; // E7.1: пауза cleanup на миссии/катсцене
char s_presenceNameBuf[96] = "";

bool UiAppActiveHash(unsigned long long appHash)
{
    using namespace natives_api;
    Init(natives::IS_UIAPP_ACTIVE_BY_HASH);
    Push(appHash);
    if (Call() != 0)
        return true;
    Init(natives::IS_UIAPP_RUNNING_BY_HASH);
    Push(appHash);
    return Call() != 0;
}

bool UiAppRunningName(const char* appName)
{
    using namespace natives_api;
    Init(natives::IS_UIAPP_RUNNING);
    Push(reinterpret_cast<unsigned long long>(const_cast<char*>(appName)));
    return Call() != 0;
}

// Эмпирический маппинг UIAPP -> биты v18 (уточняется по debug-логу ui:).
uint16_t DetectLocalPresenceUi()
{
    using namespace natives_api;
    Init(natives::IS_PAUSE_MENU_ACTIVE);
    const bool pause = Call() != 0;

    const bool pauseMenu = UiAppActiveHash(kUiAppPauseMenu) ||
                           UiAppRunningName("pause_menu");
    const bool map = UiAppActiveHash(kUiAppMap) || UiAppRunningName("map");
    const bool satchel = UiAppActiveHash(kUiAppSatchel) ||
                         UiAppRunningName("satchel");
    const bool shop = UiAppActiveHash(kUiAppShop) || UiAppRunningName("shop");
    const bool inv = UiAppActiveHash(kUiAppInventory) ||
                     UiAppRunningName("inventory");
    const bool playerMenu = UiAppActiveHash(kUiAppPlayerMenu) ||
                            UiAppRunningName("player_menu");
    const bool anyApp = pauseMenu || map || satchel || shop || inv ||
                        playerMenu;

    uint16_t flags = 0;
    if (map) flags |= net::kPlayerPresenceInMap;
    if (satchel || inv) flags |= net::kPlayerPresenceInSatchel;
    if (shop) flags |= net::kPlayerPresenceInShop;
    if (playerMenu) flags |= net::kPlayerPresenceInMenuOther;
    if (pause) flags |= net::kPlayerPresenceInPauseMenu;

    // Диагностика (npc_debug=1, 1/с, только когда пауза активна) — по ней
    // фиксируем реальную привязку UIAPP-хэшей к карте/сатчелю/магазину.
    if (coopcfg::Get().npcDebug && pause &&
        s_tickCount - s_lastUiPresenceLogTick >= kPresenceLogEveryTicks)
    {
        s_lastUiPresenceLogTick = s_tickCount;
        sh_log::Write(
            "ui: pause=%d anyApp=%d pause_menu=%d map=%d satchel=%d shop=%d "
            "inv=%d\n",
            pause ? 1 : 0, anyApp ? 1 : 0, pauseMenu ? 1 : 0, map ? 1 : 0,
            satchel ? 1 : 0, shop ? 1 : 0, inv ? 1 : 0);
    }
    return flags;
}

// Детект раз в ~5 тиков; отправка при изменении + heartbeat 2 с, <=10/с.
void UpdateLocalPresence()
{
    if (!coopcfg::Get().npcActor || !coopcfg::Get().npcPresence)
        return;
    if (!g_netStarted)
        return;
    // E6: протухший статус партнёра (нет heartbeat) — суффикс снимаем.
    if (s_remotePresenceValid &&
        s_tickCount - s_lastRemotePresenceTick >
            kPresenceRemoteTimeoutTicks)
    {
        s_remotePresenceValid = false;
        s_remotePresenceFlags = 0;
    }
    if (s_tickCount - s_lastPresenceDetectTick < kPresenceDetectPeriodTicks)
        return;
    s_lastPresenceDetectTick = s_tickCount;

    const uint16_t flags = DetectLocalPresenceUi();
    s_localPresenceFlags = flags;
    const bool changed = !s_presenceSentValid ||
                         flags != s_lastSentPresenceFlags;
    const unsigned long long presenceNowMs = GetTickCount64();
    const bool heartbeat =
        s_presenceSentValid &&
        (presenceNowMs - s_lastPresenceSendMs) >= 2000ull; // 2 c wall-clock
    if (!changed && !heartbeat)
        return;

    if (s_presenceWindowMs == 0 ||
        presenceNowMs - s_presenceWindowMs >= 1000ull)
    {
        s_presenceWindowMs = presenceNowMs;
        s_presenceWindowCount = 0;
    }
    if (s_presenceWindowCount >= kPresenceMaxPerSecond)
        return; // сервер всё равно rate-limit'ит 10/с

    if (!SendPlayerPresence(s_presenceEpoch, flags))
        return;
    ++s_presenceWindowCount;
    s_lastPresenceSendTick = s_tickCount;
    s_lastPresenceSendMs = presenceNowMs;
    s_lastSentPresenceFlags = flags;
    s_presenceSentValid = true;
}

// Суффикс имени над головой для удалённого игрока (приоритет специфичных
// экранов над общим pause-битом).
const char* PresenceSuffix(uint16_t flags)
{
    if (flags & net::kPlayerPresenceInCutscene)
        return "in cutscene";
    if (flags & net::kPlayerPresenceInMap)
        return "in map";
    if (flags & net::kPlayerPresenceInSatchel)
        return "in satchel";
    if (flags & net::kPlayerPresenceInShop)
        return "in shop";
    if (flags & net::kPlayerPresenceInPauseMenu)
        return "pause menu";
    if (flags & net::kPlayerPresenceInMenuOther)
        return "in menu";
    return "";
}

// Состояние для спавна от сервера. Пишет/читает только игровой поток
// (сетевые колбэки складывают данные в inbox выше).
float  s_pendingHeading = 0;
char   s_pendingName[64] = "";
unsigned long long s_remoteModel = 0; // модель удалённого игрока (из PLAYER_JOINED)
// P2: fallback-модель из PLAYER_JOINED (из INI/сервера), реальная модель
// персонажа приходит только кадром (reserved[2]). Храним их РАЗДЕЛЬНО:
// после первого получения реальной модели fallback больше не должен её
// перезаписывать (иначе пед пересоздаётся на каждом join/reconnect).
unsigned long long s_joinedModel = 0;
bool s_realModelSeen = false;          // реальная модель хоть раз получена
unsigned long long s_lastRealModel = 0; // последняя реальная модель персонажа
// Кулдаун пересоздания педа по смене модели — чтобы не было «дёрганья»
// пачкой (не чаще ~1.5 с).
unsigned int s_lastGhostRecreateTick = 0;
constexpr unsigned int kModelRecreateCooldownTicks = 90;
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
// P0-2 (task-156): компоновка/сентинел последнего отправленного Player Frame.
struct PlayerFrameSig
{
    float x = 0.f, y = 0.f, z = 0.f, heading = 0.f;
    float aimX = 0.f, aimY = 0.f, aimZ = 0.f;
    float health = 0.f;
    uint8_t move = 0, state = 0, flags = 0, action = 0;
    uint32_t weapon = 0, mountModel = 0;
    unsigned long long pedModel = 0;
    unsigned long long metaOutfit = 0;
};
PlayerFrameSig s_lastPlayerFrameSig{};
bool s_lastPlayerFrameSigValid = false;
unsigned int s_lastPlayerFrameSendTick = 0;
unsigned int s_lastPlayerOutfitCollectTick = 0;
unsigned int s_lastHorseOutfitCollectTick = 0;
// task-160: wall-clock ms базы (не зависят от FPS).
unsigned long long s_lastPlayerFrameSendMs = 0;
unsigned long long s_lastPlayerOutfitCollectMs = 0;
unsigned long long s_lastHorseOutfitCollectMs = 0;
unsigned long long s_lastWorldScanMs = 0;
unsigned long long s_perfWindowStartMs = 0;
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
// Очередь небольшая: если она растёт быстрее, чем призрак успевает
// проигрывать анимации, лучше отбросить старое (визуальная аппроксимация).
constexpr int kShotQueueSize = 16;
std::mutex s_shotMtx;
Vec3 s_shotQueue[kShotQueueSize];
// Время прихода события (GetTickCount64, мс) — для отбрасывания «зависших»
// событий, которые уже нельзя показать без задержки.
unsigned long long s_shotQueueMs[kShotQueueSize] = {0};
int  s_shotQCount = 0;
std::atomic<int> s_shotDropped{0}; // пишется сетевым потоком, читается игровым
std::atomic<bool> s_shotEventsPending{false};
unsigned int s_lastShotEventTaskTick = 0;
unsigned int s_lastShotTick = 0; // минимум паузы между задачами выстрела
// Реплей выстрелов — state machine с ПОДТВЕРЖДЕНИЕМ (IS_PED_SHOOTING у
// призрака): новая TASK_* заменяет предыдущую и обрывает анимацию, поэтому
// пока выстрел не подтверждён (Issued) — никаких aim/move/heading/shoot, а
// после подтверждения (Confirmed) управление возвращается. Пока ИДЁТ
// анимация per-shot, ждём подтверждения; если 30 тиков нет — 1 retry.
enum class ShotReplayPhase : uint8_t
{
    Idle = 0,   // управление свободно, можно выдать следующий выстрел
    Issued,     // задача выдана, ждём IS_PED_SHOOTING
    Confirmed,  // выстрел состоялся, ждём завершения анимации/готовности
};
ShotReplayPhase s_shotReplayPhase = ShotReplayPhase::Idle;
unsigned int s_shotReplayPhaseTick = 0; // тик входа в Issued/Confirmed
bool s_shotReplayRetried = false;       // повторная попытка для текущего события
Vec3 s_shotReplayPoint{0.f, 0.f, 0.f}; // цель текущего per-shot события
// AUTO-FIRE: если очередь >= kShotReplayBurstThreshold, не гоняем per-shot,
// а выдаём ОДИН длинный TASK_SHOOT_AT_COORD (FullAuto) + SET_PED_SHOOT_RATE.
bool s_shotReplayBurst = false;
unsigned int s_shotReplayBurstTick = 0;
unsigned int s_shotReplayBurstAimTick = 0;
Vec3 s_shotReplayBurstPoint{0.f, 0.f, 0.f};
unsigned int s_shotReplayLastEventTick = 0; // тик последнего события (для coalesce)
// Ждём «оружие готово» с таймаутом: иначе лук/не-firearm заблокирует навсегда.
unsigned int s_shotReplayReadyFailSince = 0;
// Счётчик поздних drop'ов реплея (только игровой поток) + троттлинг лога.
unsigned int s_replayDropped = 0;
unsigned int s_lastReplayDropLogTick = 0;
// Безопасный интервал между per-shot задачами: 12-15 тиков (~0.2-0.25 с).
constexpr unsigned int kShotReplayIntervalTicks = 14;
// Если IS_PED_WEAPON_READY_TO_SHOOT недоступен/висит — через это время всё
// равно выдаём задачу (не блокироваться навсегда). ВАЖНО: таймаут должен
// быть МЕНЬШЕ возраста события (kShotReplayMaxAgeMs), иначе первый выстрел
// после респавна/смены оружия/CLEAR стареет и выкидывается раньше выдачи.
constexpr unsigned int kShotReplayReadyTimeoutTicks = 12;
// Событие старше этого (по времени прихода) форсирует выдачу задачи без
// ожидания ready — чтобы не потерять первый выстрел из-за «не готов».
constexpr unsigned long long kShotReplayForceAgeMs = 250ull;
// forceByAge отменяет ТОЛЬКО ready: минимальный интервал между задачами
// всё равно соблюдаем (6 тиков ~= 100 мс), иначе две TASK_SHOOT подряд
// перезапускают анимацию.
constexpr unsigned int kShotReplayForceMinIntervalTicks = 6;
// Если выстрел не подтвердился за 30 тиков — одна retry-попытка с длинной
// анимацией, затем событие отбрасывается.
constexpr unsigned int kShotReplayConfirmTimeoutTicks = 30;
constexpr unsigned int kShotReplayConfirmMaxTicks = 120; // safety в Confirmed
// Per-shot duration (500-800) и duration для длинного авто-огня.
constexpr int kShotReplayShotDurationMs = 500;
constexpr int kShotReplayRetryDurationMs = 800;
constexpr int kShotReplayBurstDurationMs = 1500;
// На лошади длинный FullAuto-хвост не снять CLEAR'ом — режем duration.
constexpr int kShotReplayBurstDurationMountedMs = 700;
// Очередь >= порога = авто-огонь: один длинный task вместо серии коротких.
constexpr int kShotReplayBurstThreshold = 4;
constexpr unsigned int kShotReplayBurstAimIntervalTicks = 30; // ~500 мс
constexpr unsigned int kShotReplayBurstIdleTicks = 18;        // ~300 мс тишины
// Safety для burst. При непрерывном огне продлевается на каждом событии;
// 180 тиков (3 с) вызывали видимый сброс посреди стрельбы — поднято до 1200.
constexpr unsigned int kShotReplayBurstMaxTicks = 1200;
constexpr float kShotReplayBurstAimMoveDist = 2.0f;           // >2 м — обновить
// Умеренный shoot rate для burst (0..1000); после burst возвращаем ДЕФОЛТ.
// По SDK (Ped.cs:519): 100 = default, 0 = самый медленный, поэтому сброс в 0
// оставлял бы ghost медленным и per-shot после burst мог не успевать.
constexpr int kShotReplayBurstShootRate = 300;
constexpr int kGhostShootRateDefault = 100;
// Событие старше этого возраста не показываем (300-500 мс, иначе «залпы»).
constexpr unsigned long long kShotReplayMaxAgeMs = 400ull;
// Firing patterns (rdr2-runtime): SingleShot / FullAuto.
constexpr unsigned long long kShotFiringPatternSingleShot = 0x5D60E4E0ull;
constexpr unsigned long long kShotFiringPatternFullAuto = 0xC6EE6B4Cull;
// Последнее «сырое» действие удалённого игрока (для запрета CLEAR при
// сценарии/сидении). Пишется в ApplyActionToGhost (игровой поток).
uint8_t s_remoteActionRaw = 0;
// Состояние мира от хоста (время+погода). Игровой поток: забрано из inbox в
// начале Tick().
net::WorldState s_worldState{};
bool s_haveWorldState = false;
std::atomic<bool> s_amHost{false};

// E7.1 v2: модели продавцов/барменов (rdr2-hashdb/Peds.json) — сохраняем.
static const uint32_t kVendorModels[] = {
    0x0A219825u, // U_F_M_TLJBARTENDER_01
    0x0D1298F7u, // U_M_M_NBXBARTENDER_02
    0x104FE6B3u, // U_M_M_STRGENSTOREOWNER_01
    0x1EE53C9Cu, // U_M_M_NBXBARTENDER_01
    0x23DA7E9Fu, // U_M_M_RHDBARTENDER_01
    0x3E471440u, // U_F_M_VHTBARTENDER_01
    0x400E34A9u, // U_M_M_SDCUSTOMVENDOR_01
    0x42DD47BFu, // U_M_M_WALGENERALSTOREOWNER_01
    0x5339E46Bu, // S_M_M_MARKETVENDOR_01
    0x54123716u, // U_M_M_NBXGENERALSTOREOWNER_01
    0x6958B082u, // U_M_O_BLWGENERALSTOREOWNER_01
    0x893751E8u, // U_M_M_RHDGENSTOREOWNER_02
    0x8E81C857u, // U_M_O_BLWBARTENDER_01
    0x904CC489u, // U_M_O_VALBARTENDER_01
    0x9B3E551Bu, // U_M_M_VALGENSTOREOWNER_01
    0x9B9EF6B7u, // U_M_M_RHDGENSTOREOWNER_01
    0xB733CF0Fu, // U_M_M_VALBARTENDER_01
    0xC5DF0CFEu, // U_M_M_ARMGENERALSTOREOWNER_01
    0xCE72526Eu, // U_M_O_VHTEXOTICSHOPKEEPER_01
    0xD31B229Fu, // U_M_M_TUMBARTENDER_01
    0xDD6D1B34u, // U_M_O_ARMBARTENDER_01
    0xF1029EA3u, // U_M_M_SDEXOTICSSHOPKEEPER_01
    0xF1711637u, // U_F_M_TUMGENERALSTOREOWNER_01
};

bool IsVendorModel(uint32_t model)
{
    for (uint32_t m : kVendorModels)
    {
        if (m == model)
            return true;
        if (m > model)
            return false;
    }
    return false;
}

bool IsMissionEntityCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_MISSION_ENTITY);
    Push(handle);
    return Call() != 0;
}

bool IsPlayerPedCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_A_PLAYER);
    Push(handle);
    return Call() != 0;
}

uint32_t GetEntityModelCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_MODEL);
    Push(handle);
    return static_cast<uint32_t>(Call());
}

bool IsPedInAnyVehicleCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_IN_ANY_VEHICLE);
    Push(handle);
    Push(false);
    return Call() != 0;
}

// Сценарный/динамический сценарный пед (продавцы/работники/события).
bool IsPedOnMountCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_ON_MOUNT);
    Push(handle);
    return Call() != 0;
}

bool IsAnimalControlledByPlayerCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::_IS_ANIMAL_CONTROLLED_BY_A_PLAYER);
    Push(handle);
    return Call() != 0;
}

bool IsPedUsingScenarioCoop(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_USING_ANY_SCENARIO);
    Push(handle);
    if (Call() != 0)
        return true;
    Init(natives::_GET_ACTIVE_DYNAMIC_SCENARIO);
    Push(handle);
    if (Call() != 0)
        return true;
    Init(natives::_GET_ACTIVE_DYNAMIC_SCENARIO_2);
    Push(handle);
    return Call() != 0;
}

// «Чистый» ambient-пед, которого можно убрать у гостя.
bool IsRemovableAmbientPed(unsigned int handle, unsigned int localPed)
{
    if (!handle || handle == localPed)
        return false;
    if (handle == s_ghost || IsWorldSyncExcluded(handle))
        return false; // ghost/лошадь партнёра
    if (!DoesEntityExist(handle))
        return false;
    if (IsPedDeadOrDying(handle))
        return false;
    if (IsPlayerPedCoop(handle))
        return false;
    if (IsPedOnMountCoop(handle))
        return false; // всадник (не разрываем пару конь+всадник)
    if (IsAnimalControlledByPlayerCoop(handle))
        return false; // животное/лошадь игрока
    if (IsMissionEntityCoop(handle))
        return false; // mission/script/quest NPC
    if (coopcfg::Get().npcVisitorPreserveEvents)
    {
        if (IsPedUsingScenarioCoop(handle))
            return false; // продавцы/работники/события
        if (IsVendorModel(GetEntityModelCoop(handle)))
            return false;
        if (IsPedInAnyVehicleCoop(handle))
            return false; // не трогаем водителей/трафик
    }
    return true;
}

// Собрать очередь на удаление (радиус ~120 м вокруг игрока).
void QueueVisitorAmbientCleanup(unsigned int localPed)
{
    if (!sh::HasWorldQueries() || !sh::worldGetAllPeds)
        return;
    static int handles[2048];
    int count = sh::worldGetAllPeds(handles, 2048);
    if (count < 0)
        count = 0;
    if (count > 2048)
        count = 2048;
    const Vec3 center = GetEntityCoords(localPed);
    for (int i = 0; i < count && s_visitorCleanupCount < kVisitorCleanupQueueMax;
         ++i)
    {
        const unsigned int h = static_cast<unsigned int>(handles[i]);
        if (!IsRemovableAmbientPed(h, localPed))
            continue;
        const Vec3 cp = GetEntityCoords(h);
        const float dx = cp.x - center.x;
        const float dy = cp.y - center.y;
        const float dz = cp.z - center.z;
        if (dx * dx + dy * dy + dz * dz >
            kVisitorCleanupRadius * kVisitorCleanupRadius)
            continue;
        bool duplicate = false;
        for (int q = 0; q < s_visitorCleanupCount; ++q)
        {
            if (s_visitorCleanup[q].handle == h)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;
        VisitorCleanupPed e;
        e.handle = h;
        e.fade = kVisitorCleanupFadeTicks;
        s_visitorCleanup[s_visitorCleanupCount++] = e;
    }
}

// Фейд alpha -> 0 за ~0.4 c, затем DELETE_ENTITY; <=8 удалений/тик.
void ProcessVisitorCleanup()
{
    int deletions = 0;
    for (int i = 0; i < s_visitorCleanupCount;)
    {
        VisitorCleanupPed& e = s_visitorCleanup[i];
        if (!e.handle || !DoesEntityExist(e.handle))
        {
            e = s_visitorCleanup[--s_visitorCleanupCount];
            continue;
        }
        if (e.fade > 0)
        {
            using namespace natives_api;
            Init(natives::SET_ENTITY_ALPHA);
            Push(e.handle);
            Push(static_cast<unsigned long long>(
                (255u * e.fade) / kVisitorCleanupFadeTicks));
            Push(false);
            Call();
            --e.fade;
            ++i;
            continue;
        }
        if (deletions >= kVisitorCleanupDeletePerTick)
        {
            ++i;
            continue;
        }
        using namespace natives_api;
        unsigned int h = e.handle;
        Init(natives::DELETE_ENTITY);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
        if (!DoesEntityExist(e.handle))
            ++s_visitorCleanupRemoved;
        e = s_visitorCleanup[--s_visitorCleanupCount];
        ++deletions;
    }
    if (s_visitorCleanupRemoved > 0 &&
        s_tickCount - s_lastVisitorCleanupLogTick >= 60)
    {
        s_lastVisitorCleanupLogTick = s_tickCount;
        if (coopcfg::Get().npcDebug)
            sh_log::Write("actor: visitor cleanup removed=%u\n",
                          s_visitorCleanupRemoved);
        s_visitorCleanupRemoved = 0;
    }
}

// E7.1 v2: visitor mode — гость гасит ТОЛЬКО ambient-плотности (crowd/animals,
// опционально трафик), сохраняя scenario/vendor/quest/event NPC. Хост не
// подавляет; npc_actor=0/npc_visitor=0 — поведение как раньше.
void UpdateVisitorMode()
{
    const bool want = coopcfg::Get().npcActor &&
                      coopcfg::Get().npcVisitor &&
                      g_netStarted && s_joinOkSeen && !s_amHost.load();
    if (want)
    {
        const bool vehicles = coopcfg::Get().npcVisitorVehicles;
        world_sync::SuppressAmbientForVisitor(vehicles);

        // Persistent road population — только при подавлении машин; парно
        // включаем обратно при выходе из visitor mode.
        if (vehicles && !s_visitorRoadPopDisabled)
        {
            using namespace natives_api;
            Init(natives::DISABLE_AMBIENT_ROAD_POPULATION);
            Push(true);
            Call();
            s_visitorRoadPopDisabled = true;
        }

        // E7.1: mission-aware visitor — на время миссии/катсцены паузим
        // ТОЛЬКО cleanup толпы; density-подавление выше уже выполнено.
        bool sceneMission = false;
        bool sceneCutscene = false;
        bool scenePaused = false;
        if (coopcfg::Get().npcVisitorPauseScenes)
        {
            using namespace natives_api;
            Init(natives::GET_MISSION_FLAG);
            sceneMission = Call() != 0;
            Init(natives::PLAYER_ID);
            const unsigned int player = static_cast<unsigned int>(Call());
            Init(natives::IS_PLAYER_READY_FOR_CUTSCENE);
            Push(player);
            sceneCutscene = Call() != 0;
            scenePaused = sceneMission || sceneCutscene;
        }
        if (scenePaused != s_visitorScenePaused)
        {
            s_visitorScenePaused = scenePaused;
            if (scenePaused)
                s_visitorCleanupRanOnce = false; // после сцены — сразу cleanup
            if (coopcfg::Get().npcDebug)
                sh_log::Write("actor: visitor scene pause %s mission=%d "
                              "cutscene=%d\n",
                              scenePaused ? "on" : "off",
                              sceneMission ? 1 : 0, sceneCutscene ? 1 : 0);
        }

        const unsigned int localPed = GetPlayerPedFor(0);
        if (localPed && !scenePaused)
        {
            const bool due = !s_visitorCleanupRanOnce ||
                (s_tickCount - s_lastVisitorCleanupTick >=
                 kVisitorCleanupIntervalTicks);
            if (due)
            {
                s_visitorCleanupRanOnce = true;
                s_lastVisitorCleanupTick = s_tickCount;
                if (coopcfg::Get().npcVisitorPreserveEvents)
                {
                    QueueVisitorAmbientCleanup(localPed);
                }
                else
                {
                    // Агрессивный режим (opt-in): CLEAR_AREA с флагом
                    // AMBIENT_POPULATION; может задеть сценарных NPC.
                    const Vec3 c = GetEntityCoords(localPed);
                    using namespace natives_api;
                    Init(natives::CLEAR_AREA);
                    Push(c.x); Push(c.y); Push(c.z);
                    Push(kVisitorCleanupRadius);
                    Push(kClearAreaAmbientFlag);
                    Call();
                }
            }
            if (coopcfg::Get().npcVisitorPreserveEvents)
                ProcessVisitorCleanup();
        }

        if (!s_visitorModeActive)
        {
            s_visitorModeActive = true;
            if (coopcfg::Get().npcDebug)
                sh_log::Write(
                    "actor: visitor mode on (ambient suppressed, preserve "
                    "scenario/events)\n");
        }
    }
    else
    {
        if (s_visitorRoadPopDisabled)
        {
            using namespace natives_api;
            Init(natives::ENABLE_AMBIENT_ROAD_POPULATION);
            Call();
            s_visitorRoadPopDisabled = false;
        }
        s_visitorCleanupCount = 0;
        s_visitorCleanupRanOnce = false;
        s_visitorScenePaused = false;
        if (s_visitorModeActive)
        {
            s_visitorModeActive = false;
            if (coopcfg::Get().npcDebug)
                sh_log::Write("actor: visitor mode off\n");
        }
    }
}
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
// Новому педу нужен «base scrub»: после RandomizePedComponents на нём
// остаётся случайная shop-одежда (рубашка/исподнее), которая просвечивает
// сквозь куртку хоста. Флаг ставится при спавне/респавне/смене модели и
// потребляется при первом применении наряда хоста.
bool s_ghostNeedsBaseScrub = false;
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
// Сколько тиков подряд призрак далеко от цели (>3 м). Если навмеш-задача
// застряла/не дошла, через ~30 тиков включаем принудительный спринт-догон,
// а при большом разрыве — телепорт (призрак; точность важнее реализма).
unsigned int s_ghostFarTicks = 0;
// Тик последней выдачи задачи догона (не перевыдаём каждый кадр — дёргает).
unsigned int s_lastSprintTaskTick = 0;
// Сколько тиков подряд далёкая ошибка держится, прежде чем догонять спринтом.
constexpr unsigned int kGhostFarTicksForSprint = 30;
// Редкий лог телепорта призрака (не чаще раза в 5 с).
unsigned int s_lastGhostTpLogTick = 0;
// Кулдаун телепорта в Idle (30-60 тиков): защита от телепорт-петли, когда
// цель/пед «прыгают» вокруг порога (10 м).
unsigned int s_lastGhostTeleportTick = 0;
constexpr unsigned int kIdleTeleportCooldownTicks = 60;
// Тик последнего реплея наряда (heartbeat) — для редкого лога.
unsigned int s_lastOutfitReapplyLogTick = 0;
constexpr unsigned int kOutfitReapplyLogTicks = 5 * 60 * 60; // heartbeat: 5 мин
// Реальные СМЕНЫ наряда логируем чаще (диагностика расслоения), с дебаунсом:
// не чаще раза в ~1 с (смена и так дебаунсится 0.5 с у отправителя).
constexpr unsigned int kOutfitChangeLogTicks = 60;
// Лог применения wearable states (открытая/закрытая куртка) — редко.
unsigned int s_lastOutfitWearableLogTick = 0;
constexpr unsigned int kWearableLogTicks = 5 * 60 * 60; // не чаще 5 минут
// Heartbeat с идентичным набором НЕ переприменяем (это визуально «пересобирает»
// стоящего призрака). Лог скипа — редко.
unsigned int s_lastOutfitHeartbeatSkipLogTick = 0;
constexpr unsigned int kOutfitHeartbeatSkipLogTicks = 5 * 60 * 60;
// Отправитель: heartbeat оставляем (ресинк после reconnect/потерь), но лог
// прореживаем, иначе `send outfit ... [heartbeat]` засоряет лог каждые 5 с.
unsigned int s_lastOutfitHeartbeatLogTick = 0;
constexpr unsigned int kOutfitHeartbeatLogTicks = 60 * 60; // ~60 с
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
// Анимации оружия у призрака: доставание / убирание / перезарядка.
// Раньше смена weaponHash сразу делала GIVE + SET_CURRENT(equipNow=1) и REMOVE —
// мгновенно, без анимаций и звуков. Теперь переходы играются постепенно.
bool s_weaponDrawPending = false;    // идёт анимация доставания
unsigned int s_weaponDrawStartTick = 0;
bool s_weaponHolsterPending = false; // идёт анимация убирания
unsigned int s_weaponHolsterStartTick = 0;
bool s_reloadInProgress = false;     // TASK_RELOAD_WEAPON играется
unsigned int s_reloadStartTick = 0;
// В бою (TASK_SHOOT/AIM) не ждём всю анимацию доставания: форсируем экипировку
// через ~0.33 с, иначе первые выстрелы уходят в пустоту.
constexpr unsigned int kWeaponDrawForceTicks = 20;
// Без боя — латч «в руках» ставим по готовности или таймауту.
constexpr unsigned int kWeaponDrawTimeoutTicks = 90;
// Убирание: REMOVE_WEAPON_FROM_PED только после анимации/таймаута (не спамим).
constexpr unsigned int kWeaponHolsterTimeoutTicks = 75;
// Перезарядка: максимум ~5 с, минимум ~0.33 с до проверки IS_PED_RELOADING.
constexpr unsigned int kReloadTimeoutTicks = 300;
constexpr unsigned int kReloadMinTicks = 20;
// Закрытие/доставание: если _IS_WEAPON_HOLSTER_STATE_CHANGING не сбрасывается
// (натив недоступен), срабатывает таймаут.
constexpr unsigned int kWeaponChangingFallbackTicks = 20;
// Оружие у хоста: drawn/changing приходят флагами кадра (kFlagWeaponDrawn/
// kFlagWeaponChanging, v14 без поднятия версии).
bool s_remoteWeaponDrawn = false;
bool s_remoteWeaponChanging = false;
// Локально идёт смена holster-состояния (кэш на тик для гейтов).
bool s_weaponChangingNow = false;
// Защита от залипшего changing: гейтим aim/shoot/move только первые
// kWeaponChangingMaxGateTicks тиков эпизода, дальше игнорируем до спада флага.
bool s_weaponChangingPrevRaw = false;
unsigned int s_weaponChangingSinceTick = 0;
bool s_weaponChangingStuckLogged = false;
constexpr unsigned int kWeaponChangingMaxGateTicks = 180;
// Перезарядка: fallback _MAKE_PED_RELOAD, если TASK_RELOAD_WEAPON не стартовал.
bool s_reloadFallbackTried = false;
// Дебаунс смены weaponHash: отправитель может кратковременно дёргать
// 0/hash (перезарядка/доставание/посадка) — переключаем анимации только после
// стабильного удержания kWeaponStableTicks тиков, иначе draw/holster спамятся.
unsigned long long s_weaponCandidateHash = 0;
unsigned int s_weaponCandidateTicks = 0;
constexpr unsigned int kWeaponStableTicks = 8;
// Лог переходов оружия — редко (переходы могут дёргаться от отправителя).
unsigned int s_lastWeaponAnimLogTick = 0;
constexpr unsigned int kWeaponAnimLogTicks = 60; // ~1 с


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

// Активен ли сейчас реплей выстрела. Пока Issued (задача выдана, но выстрел
// ещё не подтверждён) или идёт burst-режим — НЕ выдаём TASK_AIM/TASK_SHOOT и
// не перебиваем задачу движением/телепортом/heading. После Confirmed
// управление возвращаем (выстрел уже состоялся).
bool GhostShotReplayActive()
{
    return s_shootHoldTicks > 0 ||
           s_shotReplayPhase == ShotReplayPhase::Issued || s_shotReplayBurst ||
           // Анимации оружия (доставание/убирание/перезарядка) не перебиваем
           // aim/shoot/движением: иначе анимация и звук обрываются. changing —
           // и локальный натив, и флаг от хоста.
           s_weaponDrawPending || s_weaponHolsterPending || s_reloadInProgress ||
           s_weaponChangingNow; // gated (см. kWeaponChangingMaxGateTicks)
}

// Для aim: не выдавать прицел, пока в очереди есть неразобранные выстрелы
// (aim между выстрелами — только при пустой очереди).
bool GhostAimBlocked()
{
    return GhostShotReplayActive() || s_shotEventsPending.load();
}

// Сброс sender-детектора выстрелов (ammo-латч, очередь, impact-точка).
// Определение ниже, рядом со статиками детектора; вызывается из
// ResetGhostCombatState (смерть/респавн/cleanup/model change/дисконнект),
// чтобы после сброса магазина/смерти не улетели фантомные Shot пачкой.
void ResetLocalShotDetector();

// Вернуть/выключить бесконечный клип магазина (определение ниже, после
// натив-хелперов оружия). Нужен в ResetGhostCombatState: если reset прервал
// перезарядку (clip-infinite был временно выключен), его надо вернуть.
void SetGhostInfiniteClip(bool on);

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
    // Анимации оружия не должны переживать пересоздание педа.
    s_weaponDrawPending = false;
    s_weaponHolsterPending = false;
    s_weaponCandidateHash = 0;
    s_weaponCandidateTicks = 0;
    s_remoteWeaponDrawn = false;
    s_remoteWeaponChanging = false;
    s_weaponChangingNow = false;
    s_weaponChangingPrevRaw = false;
    s_weaponChangingSinceTick = 0;
    s_weaponChangingStuckLogged = false;
    s_reloadFallbackTried = false;
    // Если reset прервал перезарядку — вернуть бесконечный клип, иначе
    // выстрелы после респавна снова начнут тратить конечный магазин.
    if (s_reloadInProgress && s_ghost && DoesEntityExist(s_ghost))
        SetGhostInfiniteClip(true);
    s_reloadInProgress = false;
    s_shotReplayPhase = ShotReplayPhase::Idle;
    s_shotReplayPhaseTick = 0;
    s_shotReplayRetried = false;
    s_shotReplayBurst = false;
    s_shotReplayBurstTick = 0;
    s_shotReplayBurstAimTick = 0;
    s_shotReplayLastEventTick = 0;
    s_shotReplayReadyFailSince = 0;
    s_replayDropped = 0;
    s_lastReplayDropLogTick = 0;
    s_pendingCombatExit = false;
    s_lastAimTaskValid = false;
    s_lastAction = 0xFF;
    s_lastPedState = 0xFF;
    // Сброс состояния догона/телепортов: новый пед/локация — начинаем заново.
    s_ghostFarTicks = 0;
    s_lastGhostTeleportTick = 0;
    {
        std::lock_guard<std::mutex> lk(s_shotMtx);
        s_shotQCount = 0;
    }
    s_shotEventsPending.store(false);
    s_shotDropped.store(0);
    // Sender-side детектор: baseline магазина и очередь фантомных Shot.
    ResetLocalShotDetector();
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
// Идёт анимация посадки/спешивания (TASK_MOUNT_ANIMAL/TASK_DISMOUNT_ANIMAL):
// обрывать задачи (CLEAR/Promote) и выдавать стрельбу в эти фазы нельзя.
bool GhostMountInTransition()
{
    return s_mountPhase == MountPhase::Mounting ||
           s_mountPhase == MountPhase::Dismounting;
}
// Телепорт лошади: при отставании >25 м (раньше было 60 — на практике
// лошадь успевала уехать за горизонт, лог `mount teleport dist=60`).
// 8..25 м догоняем задачей с галопом 3.0.
constexpr float TELEPORT_DIST_MOUNT = 25.0f;
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
void MoveMountTo(const Vec3& target, float heading)
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
        // Heading после телепорта обязательно подтягиваем к направлению
        // удалённого игрока (иначе лошадь смотрит куда попало).
        SetEntityHeading(s_mount, heading);
        // Если всадник уже в седле, он переедет вместе с лошадью; ручной
        // перенос педа в этом случае только ломает посадку.
        if (!IsGhostMounted())
        {
            SetEntityCoords(s_ghost, at);
            SetEntityHeading(s_ghost, heading);
        }
        PromoteMountToGhost();
        SetEntityHeading(s_mount, heading); // Promote мог пересчитать позу
        s_mountPhase = MountPhase::Seated;
        s_mountTaskValid = false;
        s_mountStillTicks = 0;
        s_prevMountTarget = target;
        s_prevMountTargetValid = true;
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
        SetEntityHeading(s_mount, heading);
        if (!IsGhostMounted())
        {
            SetEntityCoords(s_ghost, at);
            SetEntityHeading(s_ghost, heading);
        }
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

    // Догоняем ЗАДАЧЕЙ, а не телепортом: 8..25 м — галоп 3.0, 3..8 м — бег.
    float speed = 1.0f;
    if (dist > 8.0f)       speed = 3.0f;
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

// Принудительная задача догона (спринт/бег) в обход retarget-логики
// ApplyMoveToGhost. Нужна, когда TASK_FOLLOW застряла/не догнала и призрак
// отстал на метры: обычный retarget раз в 0.25 м ошибку не исправляет.
// speedMultiplier: 1.0 шаг, 2.0 бег, 3.0 спринт (TASK_FOLLOW + blend ratio).
void IssueGhostFollowTask(unsigned int ped, const Vec3& target, float speedMultiplier)
{
    using namespace natives_api;
    SetGhostFrozen(false);
    Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
    Push(ped);
    Push(speedMultiplier > 2.5f ? 3.0f : speedMultiplier);
    Call();
    Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
    Push(ped);
    Push(target.x); Push(target.y); Push(target.z);
    Push(speedMultiplier);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // timeout
    Push(0.1f);                                        // stoppingRange
    Push(static_cast<unsigned long long>(4));          // flags
    Push(40000.f);
    Call();
    s_moveTarget = target;
    s_moveTargetValid = true;
    s_lastSprintTaskTick = s_tickCount;
}

// Перевыдавать задачу догона каждый кадр нельзя — TASK_FOLLOW «дёргается»
// (повторная постановка даёт рывки). Перевыдаём только если задачи нет,
// цель заметно сместилась или прошло >15 тиков (задача могла застрять).
bool NeedGhostCatchupTask(const Vec3& target)
{
    if (!s_moveTargetValid) return true;
    if (std::fabs(target.x - s_moveTarget.x) > 1.0f ||
        std::fabs(target.y - s_moveTarget.y) > 1.0f)
        return true;
    return (s_tickCount - s_lastSprintTaskTick) >= 15;
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

// Текущее оружие в руках и готово к выстрелу (натив IS_PED_WEAPON_READY_TO_SHOOT).
bool GhostWeaponReadyNative()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return false;
    using namespace natives_api;
    Init(natives::IS_PED_WEAPON_READY_TO_SHOOT);
    Push(s_ghost);
    return Call() != 0;
}

// Пед проигрывает анимацию перезарядки (IS_PED_RELOADING).
bool GhostIsReloadingNative()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return false;
    using namespace natives_api;
    Init(natives::IS_PED_RELOADING);
    Push(s_ghost);
    return Call() != 0;
}

// Бесконечный клип отключает анимацию перезарядки (нечего дозаряжать) —
// на время reload выключаем clip-infinite, reserve остаётся бесконечным.
void SetGhostInfiniteClip(bool on)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    Init(natives::_SET_PED_INFINITE_AMMO_CLIP);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(on ? 1 : 0));
    Call();
}

// Лог переходов оружия (доставание/убирание/перезарядка) — редко.
void LogWeaponAnim(const char* what, unsigned long long weapon)
{
    if (s_tickCount - s_lastWeaponAnimLogTick < kWeaponAnimLogTicks)
        return;
    s_lastWeaponAnimLogTick = s_tickCount;
    sh_log::Write("coop: weapon 0x%llX %s play\n", weapon, what);
}

// Идёт ли у педа анимация смены holster-состояния (доставание/убирание).
bool GhostWeaponChangingNative()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return false;
    using namespace natives_api;
    Init(natives::_IS_WEAPON_HOLSTER_STATE_CHANGING);
    Push(s_ghost);
    return Call() != 0;
}

// Начать анимацию доставания: SET_CURRENT(equipNow=false) — движок играет
// доставание и звук; латч «в руках» ставится по готовности/окончанию смены.
void StartGhostWeaponDraw()
{
    if (!s_ghost || !DoesEntityExist(s_ghost) || !s_givenWeapon) return;
    using namespace natives_api;
    SetGhostInfiniteClip(true);
    s_weaponHolsterPending = false;
    s_weaponEquippedInHands = false;
    s_weaponEquippedHash = s_givenWeapon;
    s_weaponDrawPending = true;
    s_weaponDrawStartTick = s_tickCount;
    Init(natives::SET_CURRENT_PED_WEAPON);
    Push(s_ghost);
    Push(s_givenWeapon);
    Push(static_cast<unsigned long long>(0)); // equipNow=false -> анимация
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
    sh_log::Write("coop: weapon 0x%llX draw play\n", s_givenWeapon);
    LogWeaponAnim("draw", s_givenWeapon);
}

// Начать анимацию убирания: _HIDE_PED_WEAPONS(ghost, 2, false) — «unequip
// current weapon -> WEAPON_UNARMED; immediately=false = с анимацией».
// SET_CURRENT_PED_WEAPON(...,0) НЕ работает (0 — не валидное оружие).
// ВАЖНО: оружие НЕ удаляем — оно должно остаться в кобуре (как у хоста).
void StartGhostWeaponHolster()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    Init(natives::_HIDE_PED_WEAPONS);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(2)); // p0 (как в документации натива)
    Push(static_cast<unsigned long long>(0)); // immediately=false -> с анимацией
    Call();
    s_weaponDrawPending = false;
    s_weaponEquippedInHands = false;
    s_weaponEquippedHash = 0;
    s_weaponHolsterPending = true;
    s_weaponHolsterStartTick = s_tickCount;
    sh_log::Write("coop: weapon holster play (hide weapons, keep in holster)\n");
    LogWeaponAnim("holster", s_givenWeapon);
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
        Init(natives::SET_PED_INFINITE_AMMO);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(1));
        Push(weapon);
        Call();
        SetGhostInfiniteClip(true);
        sh_log::Write("coop: weapon 0x%llX re-given (was reset)\n", weapon);
        // Начать доставание С АНИМАЦИЕЙ (equipNow=false), а не мгновенно.
        StartGhostWeaponDraw();
        return;
    }
    // Незавершённое убирание отменяем: бой требует оружие в руках.
    if (s_weaponHolsterPending)
        s_weaponHolsterPending = false;

    // Идёт анимация доставания: латч «в руках» ставим по готовности
    // (IS_PED_WEAPON_READY_TO_SHOOT). В бою не ждём всю анимацию — через
    // kWeaponDrawForceTicks форсируем equipNow=true, иначе TASK_SHOOT/AIM
    // уходят в пустоту (первый выстрел можно потерять).
    if (s_weaponDrawPending && s_weaponEquippedHash == weapon)
    {
        if (GhostWeaponReadyNative())
        {
            s_weaponDrawPending = false;
            s_weaponEquippedInHands = true;
            return;
        }
        if ((s_tickCount - s_weaponDrawStartTick) < kWeaponDrawForceTicks)
            return; // даём анимации доставания доиграть
        Init(natives::SET_CURRENT_PED_WEAPON);
        Push(s_ghost);
        Push(weapon);
        Push(static_cast<unsigned long long>(1)); // equipNow (боевой приоритет)
        Push(static_cast<unsigned long long>(0));
        Push(static_cast<unsigned long long>(0));
        Push(static_cast<unsigned long long>(0));
        Call();
        s_weaponDrawPending = false;
        s_weaponEquippedInHands = true;
        s_weaponEquippedHash = weapon;
        sh_log::Write("coop: weapon 0x%llX draw forced (combat) after %u ticks\n",
                      weapon, s_tickCount - s_weaponDrawStartTick);
        return;
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
void ShootGhostAt(const Vec3& t, int durationMs = 250,
                  unsigned long long firingPattern = kShotFiringPatternSingleShot)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    EnsureGhostWeaponInHands();
    using namespace natives_api;
    Init(natives::TASK_SHOOT_AT_COORD);
    Push(s_ghost);
    Push(t.x); Push(t.y); Push(t.z);
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(durationMs)));
    Push(firingPattern); // SingleShot (per-shot) / FullAuto (burst)
    Push(static_cast<unsigned long long>(0));
    Call();
}

// Rate авто-огня у призрака (0..1000). После burst обязательно сбрасываем,
// иначе ghost продолжает стрелять чаще, чем нужно.
void SetGhostShootRate(int rate)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    Init(natives::SET_PED_SHOOT_RATE);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(rate)));
    Call();
}

// Применить действие (action) удалённого игрока к педу призрака.
// Вызывается из ApplyRemoteFrame (игровой поток).
// Прицеливание/стрельба/перезарядка — реальные задачи оружия: пед стоит,
// целится и стреляет (со звуком и вспышкой).
// ub-mode unit C2-2a: combat-exit latch/stand-still paso (порядок сохранён).
void UpdateGhostCombatExit(bool wasCombat, bool isCombat)
{
    using namespace natives_api;
    if (wasCombat && !isCombat)
    {
        s_pendingCombatExit = true;
        s_lastAimTaskValid = false;
    }
    if (s_pendingCombatExit && !GhostShotReplayActive())
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
    // ВАЖНО: одновременно с реплеем выстрелов aim НЕ выдаём — TASK_AIM
    // отменяет идущую TASK_SHOOT (в пачке завершался только первый выстрел).
}

void ApplyActionToGhost(const net::FramePayload& f)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    const uint8_t ac = static_cast<uint8_t>(f.action);
    s_remoteActionRaw = ac; // для запрета CLEAR при сценарии/сидении
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
    UpdateGhostCombatExit(wasCombat, isCombat);

    if (isCombat && ac == static_cast<uint8_t>(net::Action::Aiming) &&
        !GhostAimBlocked())
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
        !GhostShotReplayActive() &&
        !s_shotEventsPending.load() && // есть точные Shot-события — replay сам
        // И за последние 30 тиков Shot-события не приходили вовсе: снижаем
        // остаточный дубль «auto-выстрел + replay того же события».
        (s_shotReplayLastEventTick == 0 ||
         s_tickCount - s_shotReplayLastEventTick >= 30) &&
        s_tickCount - s_lastShootTaskTick >= 15 &&
        s_tickCount - s_lastShotEventTaskTick >= 30)
    {
        s_lastShootTaskTick = s_tickCount;
        ShootGhostForward();
        // Небольшой hold: per-shot replay ждёт его окончания, иначе возможен
        // двойной выстрел. 10 тиков (~166 мс) меньше возраста события.
        s_shootHoldTicks = 10;
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
        // ДО выстрела. Пока очередь выстрелов не пуста (или выстрел ещё не
        // подтверждён) — aim не выдаём вообще: TASK_AIM отменит shoot.
        if (GhostAimBlocked())
        {
            handled = false; // повторим прицел, когда выстрелы отыграют
            break;
        }
        AimGhostForward();
        s_lastAimTaskPoint = GhostAimPointAhead();
        s_lastAimTaskValid = true;
        s_lastAimTaskTick = s_tickCount;
        sh_log::Write("coop: action=%d → aim\n", (int)ac);
        break;
    case net::Action::Shooting:
        // Пока идёт реплей выстрелов/предыдущая shoot-анимация (или в очереди
        // есть точные Shot-события) — не перезадаём: иначе двойной выстрел.
        if (GhostAimBlocked())
        {
            handled = false;
            break;
        }
        s_lastShootTaskTick = s_tickCount;
        ShootGhostForward();
        s_shootHoldTicks = 10;
        sh_log::Write("coop: action=%d → shoot\n", (int)ac);
        break;
    case net::Action::Reloading:
        // Перезарядку выдаём ОДИН раз на переход (switch попадает сюда только
        // при смене action). Пока s_reloadInProgress, GhostShotReplayActive/
        // GhostAimBlocked подавляют aim/shoot/движение — задачу не перебиваем.
        // Бесконечный клип отключает reload-анимацию (нечего дозаряжать) —
        // на время анимации выключаем clip-infinite, reserve остаётся.
        if (s_reloadInProgress)
        {
            handled = false; // уже играется — не перевыдаём каждый кадр
            break;
        }
        if (!s_weaponEquippedInHands || !s_givenWeapon || s_weaponDrawPending ||
            s_weaponHolsterPending || s_weaponChangingNow)
        {
            handled = false; // оружие достаётся/убирается — повторим позже
            break;
        }
        SetGhostInfiniteClip(false);
        // Магазин у призрака всегда полный из-за infinite-clip — без пустого
        // клипа TASK_RELOAD_WEAPON молча ничего не делает (нет анимации/звука).
        // По консультации: опустить магазин в 0, затем reload; reserve
        // остаётся бесконечным (SET_PED_INFINITE_AMMO).
        Init(natives::SET_AMMO_IN_CLIP);
        Push(s_ghost);
        Push(s_givenWeapon);
        Push(static_cast<unsigned long long>(0)); // ammo = 0
        Call();
        Init(natives::TASK_RELOAD_WEAPON);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(1)); // p1 (как в SDK)
        Call();
        s_reloadInProgress = true;
        s_reloadStartTick = s_tickCount;
        s_reloadFallbackTried = false;
        sh_log::Write("coop: action=%d → reload play\n", (int)ac);
        LogWeaponAnim("reload", s_givenWeapon);
        break;
    default:
        sh_log::Write("coop: action=%d → none\n", (int)ac);
        break;
    }
    if (handled)
        s_lastAction = ac;
}

// Доиграть анимации оружия: доставание (латч по готовности/таймауту),
// убирание (REMOVE_WEAPON_FROM_PED после анимации), перезарядка (вернуть
// бесконечный клип после IS_PED_RELOADING=false/таймаута). Вызывается из Tick.
void UpdateGhostWeaponAnim()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    // Кэш «идёт смена holster-состояния» для гейтов aim/shoot/move:
    // локальный натив + флаг kFlagWeaponChanging от хоста. Защита от залипания:
    // гейтим только первые kWeaponChangingMaxGateTicks тиков эпизода.
    const bool rawChanging = GhostWeaponChangingNative() || s_remoteWeaponChanging;
    if (rawChanging && !s_weaponChangingPrevRaw)
    {
        s_weaponChangingSinceTick = s_tickCount; // новый эпизод changing
        s_weaponChangingStuckLogged = false;
    }
    s_weaponChangingPrevRaw = rawChanging;
    s_weaponChangingNow = rawChanging &&
        (s_tickCount - s_weaponChangingSinceTick) < kWeaponChangingMaxGateTicks;
    if (rawChanging && !s_weaponChangingNow && !s_weaponChangingStuckLogged)
    {
        s_weaponChangingStuckLogged = true;
        sh_log::Write("coop: weapon changing stuck >%u ticks — ignored for gating\n",
                      kWeaponChangingMaxGateTicks);
    }

    // Доставание: снимаем pending по _IS_WEAPON_HOLSTER_STATE_CHANGING==false
    // (+ ready или таймаут), иначе по жёсткому таймауту.
    if (s_weaponDrawPending)
    {
        const unsigned int elapsed = s_tickCount - s_weaponDrawStartTick;
        const bool ready = GhostWeaponReadyNative();
        if ((!s_weaponChangingNow && (ready || elapsed >= kWeaponChangingFallbackTicks)) ||
            elapsed >= kWeaponDrawTimeoutTicks)
        {
            s_weaponDrawPending = false;
            // НЕ ставим латч при таймауте: если ready=false, SET_CURRENT с
            // equipNow=false мог не экипировать — Ensure должен форсировать.
            s_weaponEquippedInHands = ready;
            sh_log::Write("coop: weapon 0x%llX draw done (ready=%d, changing=%d)\n",
                          s_weaponEquippedHash, ready ? 1 : 0,
                          s_weaponChangingNow ? 1 : 0);
        }
    }
    // Убирание: pending снимаем по окончании смены/таймауту. Оружие НЕ удаляем
    // (по консультации worldsync): оно должно остаться в кобуре, как у хоста.
    if (s_weaponHolsterPending)
    {
        const unsigned int elapsed = s_tickCount - s_weaponHolsterStartTick;
        if ((!s_weaponChangingNow && elapsed >= kWeaponChangingFallbackTicks) ||
            elapsed >= kWeaponHolsterTimeoutTicks)
        {
            s_weaponHolsterPending = false;
            InvalidateGhostWeaponLatch();
            sh_log::Write("coop: weapon 0x%llX holster done (kept in holster)\n",
                          s_givenWeapon);
            LogWeaponAnim("holster done", s_givenWeapon);
        }
    }
    // Перезарядка: если TASK_RELOAD_WEAPON не стартовал — fallback
    // _MAKE_PED_RELOAD. Завершение по IS_PED_RELOADING=false или таймауту,
    // затем вернуть бесконечный клип (выстрелы не должны теряться).
    if (s_reloadInProgress)
    {
        const unsigned int elapsed = s_tickCount - s_reloadStartTick;
        const bool reloading = GhostIsReloadingNative();
        if (!reloading && elapsed >= 10 && !s_reloadFallbackTried)
        {
            s_reloadFallbackTried = true;
            Init(natives::_MAKE_PED_RELOAD);
            Push(s_ghost);
            Call();
            sh_log::Write("coop: reload fallback _MAKE_PED_RELOAD\n");
        }
        const unsigned int finishAt = s_reloadFallbackTried
            ? 10 + kWeaponChangingFallbackTicks
            : kReloadMinTicks;
        if ((!reloading && elapsed >= finishAt) ||
            elapsed >= kReloadTimeoutTicks)
        {
            s_reloadInProgress = false;
            s_reloadFallbackTried = false;
            SetGhostInfiniteClip(true);
            sh_log::Write("coop: reload done (restored infinite clip)\n");
        }
    }
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

    // ===== Оружие: анимации доставания/убирания (флаги кадра v14) =====
    // Хост шлёт стабильный selectedHash (_GET_PED_CURRENT_HELD_WEAPON) и флаги:
    // kFlagWeaponDrawn (в руках), kFlagWeaponChanging (идёт своя анимация).
    // Доставание: GIVE + SET_CURRENT(equipNow=false) — анимация/звук.
    // Убирание: _HIDE_PED_WEAPONS(ghost,2,false) — анимация; оружие в кобуре
    // НЕ удаляем (иначе у призрака пропадёт оружие, которое у хоста видно).
    const bool weaponDrawn = (f.flags & net::kFlagWeaponDrawn) != 0;
    const bool weaponChanging = (f.flags & net::kFlagWeaponChanging) != 0;
    s_remoteWeaponChanging = weaponChanging;

    // 1) Смена выбранного оружия (с дебаунсом от 0<->hash дёрганья).
    bool weaponChangeReady = false;
    bool weaponGaveNew = false;
    if (f.weaponHash != s_lastWeapon)
    {
        if (f.weaponHash == s_weaponCandidateHash)
            ++s_weaponCandidateTicks;
        else
        {
            s_weaponCandidateHash = f.weaponHash;
            s_weaponCandidateTicks = 1;
        }
        if (s_weaponCandidateTicks >= kWeaponStableTicks)
        {
            weaponChangeReady = true;
            s_lastWeapon = f.weaponHash;
            s_weaponCandidateHash = 0;
            s_weaponCandidateTicks = 0;
        }
    }
    if (weaponChangeReady && f.weaponHash)
    {
        // weaponHash приходит из сети — в GIVE пускаем только валидный хэш.
        if (!IsWeaponHashValid(f.weaponHash))
        {
            sh_log::Write("coop: ignoring invalid weapon hash 0x%llX\n",
                          f.weaponHash);
        }
        else
        {
            // GIVE пропускаем, если это оружие уже выдано (P3).
            if (s_givenWeapon != f.weaponHash)
            {
                // Старое оружие удаляем ТОЛЬКО при реальной смене на другое.
                // После holster (hash тот же/0) REMOVE не делаем.
                if (s_givenWeapon)
                {
                    Init(natives::REMOVE_WEAPON_FROM_PED);
                    Push(s_ghost);
                    Push(s_givenWeapon);
                    Push(static_cast<unsigned long long>(0)); // p2
                    Push(static_cast<unsigned long long>(0)); // removeReason
                    Call();
                    sh_log::Write("coop: weapon 0x%llX removed (switched to 0x%llX)\n",
                                  s_givenWeapon, f.weaponHash);
                }
                weaponGaveNew = true;
                Init(natives::GIVE_WEAPON_TO_PED);
                Push(s_ghost);
                Push(f.weaponHash);
                Push(static_cast<unsigned long long>(60)); // ammoCount
                Push(static_cast<unsigned long long>(0));  // bForceInHand (0: анимация)
                Push(static_cast<unsigned long long>(weaponDrawn ? 0 : 1)); // bForceInHolster
                Push(static_cast<unsigned long long>(0));  // attachPoint = HAND_PRIMARY
                Push(static_cast<unsigned long long>(0));  // bAllowMultipleCopies
                Push(0.5f);                                // p7 (как в R* скриптах)
                Push(1.0f);                                // p8
                Push(static_cast<unsigned long long>(0));  // addReason
                Push(static_cast<unsigned long long>(1));  // bIgnoreUnlocks
                Push(0.5f);                                // permanentDegradation
                Push(static_cast<unsigned long long>(0));  // p12
                Call();
            }
            s_givenWeapon = f.weaponHash;
            s_weaponEquippedInHands = false;
            s_weaponEquippedHash = 0;
            s_weaponDrawPending = false;
            s_weaponHolsterPending = false;
            s_reloadInProgress = false;
            s_reloadFallbackTried = false;
            Init(natives::SET_PED_INFINITE_AMMO);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(1));
            Push(f.weaponHash);
            Call();
            SetGhostInfiniteClip(true);
            sh_log::Write("coop: weapon 0x%llX given (drawn=%d, changing=%d)\n",
                          f.weaponHash, weaponDrawn ? 1 : 0,
                          weaponChanging ? 1 : 0);
            // Один refresh внешности при РЕАЛЬНОЙ смене оружия (не на каждый
            // draw/holster) и не во время посадки/спешивания.
            if (!GhostMountInTransition())
            {
                RefreshGhostComponents();
                s_outfitRefreshTicks = 90;
            }
        }
    }

    // 3) P3: смена ствола во время changing хоста — если оружие должно быть в
    // руках (drawn), а draw не запущен (pending снят/сменился ствол), досылаем.
    if (weaponDrawn && s_givenWeapon && !s_weaponDrawPending &&
        !s_weaponHolsterPending && s_weaponEquippedHash != s_givenWeapon)
        StartGhostWeaponDraw();

    // 2) Зеркалим смену drawn/holster-состояния хоста, когда он сам не в
    // процессе смены (иначе анимация хоста обгонит нашу).
    if (!weaponChanging && s_givenWeapon)
    {
        // weaponGaveNew: сменился ствол (даже если drawn не менялся) — всё
        // равно нужна анимация доставания/убирания для нового оружия.
        if (weaponDrawn && (!s_remoteWeaponDrawn || weaponGaveNew))
            StartGhostWeaponDraw();
        // P3: holster только если оружие РЕАЛЬНО было в руках.
        else if (!weaponDrawn && (s_weaponEquippedInHands || s_remoteWeaponDrawn))
            StartGhostWeaponHolster();
    }
    if (!weaponChanging)
        s_remoteWeaponDrawn = weaponDrawn;

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
                                  : ((s_realModelSeen && s_lastRealModel)
                                        ? s_lastRealModel
                                        : (s_remoteModel ? s_remoteModel
                                                         : GHOST_PED_MODEL));
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
    // После вызова inA = BOOL outStatus, inB = Hash outWearableState (низкие
    // 4 байта первого слота) — состояние слоя (открытая/закрытая куртка и
    // т.п.). Раньше значения не читались, и у призрака был дефолтный слой:
    // рубашка просвечивала/пропадала не так, как у оригинала.
    static unsigned long long inA[4] = {0, 0, 0, 0};
    static unsigned long long inB[4] = {0, 0, 0, 0};

    // Сначала обнуляем wearable (вызывающий может переиспользовать структуру).
    for (uint32_t i = 0; i < net::kMaxOutfitComponents; ++i)
        out.wearable[i] = 0;

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
        const uint32_t comp = static_cast<uint32_t>(Call());
        if (comp)
        {
            out.components[n] = comp;
            // outWearableState — hash в младших 4 байтах первого слота inB.
            out.wearable[n] = static_cast<uint32_t>(inB[0] & 0xFFFFFFFFull);
            ++n;
        }
    }
    out.count = static_cast<uint32_t>(n);
}

// Удалить ВСЕ shop-предметы, которые сейчас надеты на педа (случайная базовая
// одежда после RandomizePedComponents), и обновить variation. Делаем до 2
// проходов с ПЕРЕЧИТЫВАНИЕМ: после первого удаления движок может вернуть
// часть базовых слоёв, второй проход добивает остатки.
// Возвращает суммарное число удалённых. Вызывается ОДИН раз при первом
// применении наряда на нового педа («base scrub»), чтобы случайная рубашка
// не просвечивала сквозь куртку хоста.
uint32_t ScrubGhostShopItems()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return 0;
    using namespace natives_api;
    static unsigned long long inA[4] = {0, 0, 0, 0};
    static unsigned long long inB[4] = {0, 0, 0, 0};
    uint32_t removed = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        Init(natives::GET_NUM_COMPONENT_CATEGORIES_IN_PED);
        Push(s_ghost);
        int categories = static_cast<int>(Call());
        if (categories <= 0) break;
        if (categories > net::kMaxOutfitComponents)
            categories = net::kMaxOutfitComponents;

        uint32_t removedThisPass = 0;
        for (int i = 0; i < categories; ++i)
        {
            std::memset(inA, 0, sizeof(inA));
            std::memset(inB, 0, sizeof(inB));
            Init(natives::GET_SHOP_ITEM_COMPONENT_AT_INDEX);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(i));
            Push(static_cast<unsigned long long>(1)); // p2 = true (rdr2-runtime)
            Push(reinterpret_cast<unsigned long long>(inA));
            Push(reinterpret_cast<unsigned long long>(inB));
            const uint32_t comp = static_cast<uint32_t>(Call());
            if (!comp) continue;
            Init(natives::_REMOVE_SHOP_ITEM_FROM_PED);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(comp));
            Push(static_cast<unsigned long long>(0)); // p2
            Push(static_cast<unsigned long long>(0)); // p3
            Call();
            ++removedThisPass;
        }
        removed += removedThisPass;
        if (removedThisPass == 0)
            break; // больше нечего удалять
        // UPDATE после прохода: применяем удаление, второй проход увидит
        // реальное состояние педа.
        Init(natives::UPDATE_PED_VARIATION);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(0)); // p1
        Push(static_cast<unsigned long long>(1)); // p2
        Push(static_cast<unsigned long long>(1)); // p3
        Push(static_cast<unsigned long long>(1)); // p4
        Push(static_cast<unsigned long long>(0)); // p5
        Call();
    }
    return removed;
}

// Surgical fallback (когда набор хоста неполный/модель не совпала): не снимаем
// ВСЁ (иначе можно получить голого педа), а удаляем по категориям только те
// слоты, которые есть в наборе хоста: REMOVE_SHOP_ITEM_FROM_PED_BY_CATEGORY +
// _GET_SHOP_ITEM_COMPONENT_CATEGORY(hostItem, metapedType, isMP=false).
// Так убирается случайная рубашка, но базовые слои не трогаются.
uint32_t SurgicalRemoveShopCategories(const net::OutfitState& os)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return 0;
    using namespace natives_api;
    int metaPedType = 0;
    Init(natives::_GET_META_PED_TYPE);
    Push(s_ghost);
    metaPedType = static_cast<int>(Call());
    uint32_t removed = 0;
    for (uint32_t i = 0; i < os.count && i < net::kMaxOutfitComponents; ++i)
    {
        const uint32_t comp = os.components[i];
        if (!comp) continue;
        Init(natives::_GET_SHOP_ITEM_COMPONENT_CATEGORY);
        Push(static_cast<unsigned long long>(comp));
        Push(static_cast<unsigned long long>(
            static_cast<unsigned int>(metaPedType)));
        Push(static_cast<unsigned long long>(0)); // isMP = false
        const uint32_t category = static_cast<uint32_t>(Call());
        if (!category) continue;
        Init(natives::REMOVE_SHOP_ITEM_FROM_PED_BY_CATEGORY);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(category));
        Push(static_cast<unsigned long long>(0)); // p2
        Push(static_cast<unsigned long long>(0)); // p3
        Call();
        ++removed;
    }
    return removed;
}

// Пара из nativedb для «fix missing outfit changes»: пометить компоненты
// metaped обновлёнными, затем UPDATE_PED_VARIATION. Используется после
// remove/apply наряда (и при heartbeat-реплее).
void CommitGhostVariation()
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    using namespace natives_api;
    Init(natives::_SET_ACTIVE_META_PED_COMPONENTS_UPDATED);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(0)); // isMP = false
    Call();
    Init(natives::UPDATE_PED_VARIATION);
    Push(s_ghost);
    Push(static_cast<unsigned long long>(0)); // p1=false: не сбрасывать лицо/волосы
    Push(static_cast<unsigned long long>(1)); // p2
    Push(static_cast<unsigned long long>(1)); // p3
    Push(static_cast<unsigned long long>(1)); // p4
    Push(static_cast<unsigned long long>(0)); // p5
    Call();
}

// Применить wearable states (открытая/закрытая куртка, рукава и т.п.) для
// компонентов наряда: _UPDATE_SHOP_ITEM_WEARABLE_STATE(ped, comp, state, 0,
// false, 1). Вызывать ПОСЛЕ APPLY_SHOP_ITEM_TO_PED и ДО CommitGhostVariation.
// Возвращает число применённых состояний.
uint32_t ApplyGhostWearableStates(const net::OutfitState& os)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return 0;
    using namespace natives_api;
    uint32_t applied = 0;
    for (uint32_t i = 0; i < os.count && i < net::kMaxOutfitComponents; ++i)
    {
        const uint32_t comp = os.components[i];
        const uint32_t wearable = os.wearable[i];
        if (!comp || !wearable) continue;
        Init(natives::_UPDATE_SHOP_ITEM_WEARABLE_STATE);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(comp));
        Push(static_cast<unsigned long long>(wearable));
        Push(static_cast<unsigned long long>(0)); // p3
        Push(static_cast<unsigned long long>(0)); // p4 = false
        Push(static_cast<unsigned long long>(1)); // p5 = 1
        Call();
        ++applied;
    }
    if (applied > 0 &&
        (s_tickCount - s_lastOutfitWearableLogTick) >= kWearableLogTicks)
    {
        s_lastOutfitWearableLogTick = s_tickCount;
        sh_log::Write("coop: outfit wearable applied=%u\n", applied);
    }
    return applied;
}

// Одинаковые ли наборы компонентов (с учётом модели и wearable states).
// Сравниваем БЕЗ учёта порядка: движок отдаёт категории в разном порядке
// между кадрами/тиками, из-за чего наряд считался «изменённым»,
// переотправлялся и переприменялся (лишние слои одежды / случайный вид).
// Wearable (открытая/закрытая куртка и т.п.) сравниваем парой с компонентом:
// иначе изменения состояния слоя не отправятся/не применятся.
bool OutfitsEqual(const net::OutfitState& a, const net::OutfitState& b)
{
    if (a.count != b.count) return false;
    if (a.model && b.model && a.model != b.model) return false;
    if (a.count > static_cast<uint32_t>(net::kMaxOutfitComponents) ||
        b.count > static_cast<uint32_t>(net::kMaxOutfitComponents))
        return false;
    if (a.count == 0) return true;
    std::vector<std::pair<uint32_t, uint32_t>> ca, cb;
    ca.reserve(a.count);
    cb.reserve(b.count);
    for (uint32_t i = 0; i < a.count; ++i)
        ca.emplace_back(a.components[i], a.wearable[i]);
    for (uint32_t i = 0; i < b.count; ++i)
        cb.emplace_back(b.components[i], b.wearable[i]);
    std::sort(ca.begin(), ca.end());
    std::sort(cb.begin(), cb.end());
    return ca == cb;
}

// Применить надетые shop-предметы (одежду) удалённого игрока к педу призрака
// через _APPLY_SHOP_ITEM_TO_PED + _UPDATE_PED_VARIATION (как в rdr2-runtime
// ApplyShopItemComponents). Вызывается из Tick() (игровой поток).
// ub-mode unit C2-4: одно место применения shop-компонентов наряда.
void ApplyGhostOutfitComponents(const net::OutfitState& os, uint32_t& applied)
{
    using namespace natives_api;
    for (uint32_t i = 0; i < os.count && i < net::kMaxOutfitComponents; ++i)
    {
        const uint32_t c = os.components[i];
        if (!c) continue;
        Init(natives::APPLY_SHOP_ITEM_TO_PED);
        Push(s_ghost);
        Push(static_cast<unsigned long long>(c));
        Push(static_cast<unsigned long long>(1)); // immediately = true
        Push(static_cast<unsigned long long>(0)); // isMp = false
        Push(static_cast<unsigned long long>(1)); // p4 = true
        Call();
        ++applied;
    }
}

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
    // Базовая вариация — ОДИН раз на педа (создание/респавн). Повторная
    // рандомизация при каждом применении наряда/refresh подмешивала случайные
    // компоненты (в категориях, где у оригинала пусто), и наряд расходился
    // с оригиналом. Индивидуальные shop-предметы применяем поверх.
    bool didBaseScrub = false;
    bool baseScrubFull = false;
    uint32_t removed = 0;
    uint32_t applied = 0;
    if (!s_ghostBaseRandomized)
    {
        RandomizePedComponents(s_ghost);
        s_ghostBaseRandomized = true;
    }
    // Base scrub — только на новом педе (флаг ставится при спавне/респавне/
    // смене модели), не на heartbeat/refresh.
    if (s_ghostNeedsBaseScrub)
    {
        // Guard: полный scrub «снять всё» допустим только если модель
        // совпала и набор хоста полный (>=8 предметов). Иначе можно получить
        // голого педа — делаем surgical remove по категориям хоста.
        baseScrubFull = (os.model == s_ghostModel) && (os.count >= 8);
        if (baseScrubFull)
        {
            // Случайная базовая одежда (рубашка/исподнее) остаётся слоями и
            // просвечивает сквозь куртку хоста. Снимаем ВСЕ текущие shop-
            // предметы педа, один UPDATE (внутри ScrubGhostShopItems).
            removed = ScrubGhostShopItems();
        }
        else
        {
            removed = SurgicalRemoveShopCategories(os);
        }
        didBaseScrub = true;
        s_ghostNeedsBaseScrub = false;
        s_appliedOutfitValid = false; // набор прошлого педа к новому не относится
    }
    SetEntityVisibleForce(s_ghost);

    // Тот же набор, что уже надет (heartbeat/refresh после сброса variation):
    // НИКАКОГО scrub/remove — только переприменяем компоненты.
    const bool sameSet = !didBaseScrub && (os.count > 0) &&
                         (s_appliedOutfitValid || s_appliedOutfit.count > 0) &&
                         OutfitsEqual(os, s_appliedOutfit);
    if (sameSet)
    {
        // Набор и wearable идентичны уже надетому. Если НЕТ активного окна
        // refresh (посадка/спешивание/смена оружия, где движок сбрасывает
        // variation) — НИЧЕГО не применяем: heartbeat каждые 5 с иначе делал
        // полный reapply 22-28 shop-предметов + CommitGhostVariation, и
        // стоящий призрак визуально «пересобирался»/дёргался. Только латч.
        if (s_outfitRefreshTicks == 0)
        {
            s_appliedOutfit = os;
            s_appliedOutfitValid = true;
            if (s_tickCount - s_lastOutfitHeartbeatSkipLogTick >=
                kOutfitHeartbeatSkipLogTicks)
            {
                s_lastOutfitHeartbeatSkipLogTick = s_tickCount;
                sh_log::Write("coop: outfit heartbeat skipped (same set)\n");
            }
            return;
        }
        ApplyGhostOutfitComponents(os, applied);
        // Heartbeat/refresh: только reapply, без scrub/remove.
        // Wearable states (куртка открыта/закрыта) применяем до UPDATE.
        ApplyGhostWearableStates(os);
        CommitGhostVariation();
        s_appliedOutfit = os;
        s_appliedOutfitValid = true;
        // Лог heartbeat-реплея — не чаще 5 минут (иначе засоряет лог).
        if (s_tickCount - s_lastOutfitReapplyLogTick >= kOutfitReapplyLogTicks)
        {
            s_lastOutfitReapplyLogTick = s_tickCount;
            sh_log::Write("coop: outfit reapply removed=0 applied=%u "
                          "(heartbeat)\n", applied);
        }
        return;
    }

    // Набор ИЗМЕНИЛСЯ (и это не первый apply после scrub). Удаляем ВСЕ
    // shop-предметы предыдущего s_appliedOutfit: нативный
    // APPLY_SHOP_ITEM_TO_PED наслаивает одежду, из-за чего сквозь куртку
    // просвечивало старое исподнее/рубашка. Надеваем новый набор заново.
    if (!didBaseScrub &&
        (s_appliedOutfitValid || s_appliedOutfit.count > 0))
    {
        for (uint32_t i = 0; i < s_appliedOutfit.count &&
                             i < net::kMaxOutfitComponents; ++i)
        {
            const uint32_t oldC = s_appliedOutfit.components[i];
            if (!oldC) continue;
            Init(natives::_REMOVE_SHOP_ITEM_FROM_PED);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(oldC));
            Push(static_cast<unsigned long long>(0)); // p2
            Push(static_cast<unsigned long long>(0)); // p3
            Call();
            ++removed;
        }
    }

    // Применяем новый набор в ИСХОДНОМ порядке (components идут по индексу
    // категории от отправителя: от нижних слоёв к верхним), НЕ сортируем.
    ApplyGhostOutfitComponents(os, applied);
    // Один UPDATE в конце — без повторной RandomizePedComponents; пара
    // _SET_ACTIVE_META_PED_COMPONENTS_UPDATED + UPDATE_PED_VARIATION из nativedb.
    // Wearable states (открытая/закрытая куртка) — до финального UPDATE.
    ApplyGhostWearableStates(os);
    CommitGhostVariation();
    if (didBaseScrub)
    {
        // Событие редкое (первый apply на нового педа) — логируем всегда.
        sh_log::Write("coop: outfit base scrub removed=%u applied=%u%s\n",
                      removed, applied, baseScrubFull ? "" : " (surgical)");
    }
    else if (s_tickCount - s_lastOutfitReapplyLogTick >= kOutfitChangeLogTicks)
    {
        // Реальная смена наряда: логируем чаще (дебаунс ~1 с) для диагностики.
        s_lastOutfitReapplyLogTick = s_tickCount;
        sh_log::Write("coop: outfit reapply changed removed=%u applied=%u\n",
                      removed, applied);
    }
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
    s_ghostNeedsBaseScrub = true;
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

// ub-mode unit B1: ранние guard'ы ApplyRemoteFrame (порядок сохранён).
bool ApplyRemoteFrameEarly(const net::FramePayload& f)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return true;
    if (s_ghostDead)
    {
        if (s_deadGuardTicks == 0 && f.health > 0.2f)
            RespawnGhostPed(&f);
        else
            return true; // всё ещё мёртв — труп не трогаем
    }
    if (s_remoteInVehicle)
    {
        ApplyPedStateToGhost(f);
        ApplyPlayerOutfitToGhost(f.reserved[1]);
        return true;
    }
    return false;
}

// ub-mode unit B2: mounted/dis mount/ride-loss фазы ApplyRemoteFrame.
bool ApplyRemoteFrameMountPhase(const net::FramePayload& f, Vec3 p,
                                uint8_t move, uint8_t rideState)
{
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
        return true;
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
            return true;
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
            return true; // пока пед садится — лошадь не двигаем
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
        MoveMountTo(p, f.heading);
        // Пока лошадь ведёт задача, НЕ перебиваем heading каждый кадр:
        // TASK_FOLLOW сам разворачивает корпус, а ручной SetEntityHeading
        // дёргает лошадь на галопе. Heading правим только когда лошадь
        // почти на месте кадра (стоячая/медленная езда).
        {
            Vec3 mountPos = GetEntityCoords(s_mount);
            const float mdx = p.x - mountPos.x;
            const float mdy = p.y - mountPos.y;
            if (mdx * mdx + mdy * mdy < 1.5f * 1.5f)
            {
                // Мёртвая зона 5° — иначе ручной SetEntityHeading каждый кадр
                // дёргает лошадь мелкими коррекциями.
                float mh = f.heading - GetEntityHeading(s_mount);
                while (mh > 180.f) mh -= 360.f;
                while (mh < -180.f) mh += 360.f;
                if (std::fabs(mh) > 5.0f)
                    SetEntityHeading(s_mount, f.heading);
            }
        }
        DisableMountLocalCollision();
        ApplyPedStateToGhost(f);
        return true;
    }

    // Кадр «не верхом», но лошадь ещё есть.
    if (s_mount && DoesEntityExist(s_mount))
    {
        if (s_rideLossTicks >= kRideLossTicks)
            StartDismounting(); // запустит анимацию спешивания
        return true;                 // мерцание — продолжаем держать посадку
    }
    return false;
}

// ub-mode unit B3: position phase (air/walk/run/idle) ApplyRemoteFrame.
bool ApplyRemoteFramePositionPhase(const net::FramePayload& f, Vec3 cur,
                                   Vec3 p, uint8_t move, float teleportDist)
{
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
        if (dist > teleportDist)
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
        return true;
    }

    if (move == static_cast<uint8_t>(coop::MoveState::Walk) ||
        move == static_cast<uint8_t>(coop::MoveState::Run))
    {
        // Диагностика: редкий лог прихода кадра движения.
        if (s_tickCount % (3 * 60) == 1)
            sh_log::Write("coop: recv move=%u state=%u at (%.1f %.1f %.1f), cur=(%.1f %.1f %.1f)\n",
                          move, f.state, p.x, p.y, p.z, cur.x, cur.y, cur.z);
        // Общий расчёт ошибки слежения каждый кадр. Навмеш-задача может
        // застрять/не дойти — без коррекции призрак оставался в десятках
        // метров от оригинала, пока retarget лишь перенаправлял TASK_FOLLOW.
        float dx = p.x - cur.x, dy = p.y - cur.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > 3.0f)
            ++s_ghostFarTicks;
        else
            s_ghostFarTicks = 0;
        // Большой разрыв — жёсткий телепорт (призрак; точность важнее
        // пешего реализма), затем продолжаем движение задачей.
        // В бою (прицел/выстрел/перезарядка) НЕ телепортируем: иначе рвётся
        // визуал aim/shoot (позиция педа меняется под активной задачей).
        // Пока идёт реплей выстрелов — не телепортируем и не дёргаем движение:
        // новая задача отменяет идущую shoot-анимацию.
        const bool canTeleport = !s_combatActive && !GhostShotReplayActive();
        if (dist > 12.0f && canTeleport)
        {
            SetEntityCoords(s_ghost, p);
            cur = p;
            dist = 0.f;
            s_moveTargetValid = false;
            s_ghostFarTicks = 0;
            s_lastGhostTeleportTick = s_tickCount;
            if (s_tickCount - s_lastGhostTpLogTick > 5 * 60)
            {
                s_lastGhostTpLogTick = s_tickCount;
                sh_log::Write("coop: ghost teleport (move=%u dist>12)\n", move);
            }
        }
        const bool canMove = canTeleport;
        const bool lagging = (dist > 6.0f) ||
            (s_ghostFarTicks >= kGhostFarTicksForSprint && dist > 2.5f);
        if (canMove && lagging)
        {
            // Отстали/застряли — догон спринтом (3.0), а не обычный retarget.
            if (NeedGhostCatchupTask(p))
                IssueGhostFollowTask(s_ghost, p, 3.0f);
            s_lastMoveState = move;
            s_ghostFarTicks = 0;
        }
        else
        {
            // НЕ прижимаем педа к земле во время движения: TASK_FOLLOW_NAV_MESH
            // сам ведёт по навмешу (по земле). Ручной SetEntityCoords здесь —
            // источник багов: GET_GROUND_Z_FOR_3D_COORD на движущемся педе даёт
            // нестабильную высоту, пед «уходит под землю» и дёргается каждым
            // кадром (лог lowered спамил 40.7 -> 40.7). Прижим — только в Idle.
            ApplyMoveToGhost(s_ghost, move, p);
        }
    }
    else
    {
        // Idle: пед стоит задачей TASK_STAND_STILL. НЕ двигаем его руками
        // (шаг за тик) и НЕ дёргаем заморозкой — это и было причиной
        // «дёрганья в покое». Догон — только при реальном отставании:
        //   > 4 м  — бегом (2.0);
        //   > 10 м — телепорт по XY/Z к точке кадра (раньше сохраняли cur.z,
        //            из-за чего призрак оставался на высоте старой позиции);
        //   ошибка >30 тиков — принудительный спринт-догон (3.0).
        const float CLOSE = 1.5f;
        const float RESUME = 2.5f;
        float dx = p.x - cur.x, dy = p.y - cur.y, dz = p.z - cur.z;
        float dist = std::sqrt(dx * dx + dy * dy);
        const float dist3 = std::sqrt(dx * dx + dy * dy + dz * dz);
        // В бою не телепортируем + кулдаун от телепорт-петли вокруг порога.
        // Пока идёт реплей выстрелов — не телепортируем и не дёргаем движение:
        // новая задача отменяет идущую shoot-анимацию.
        const bool canTeleport = !s_combatActive && !GhostShotReplayActive();
        if (dist3 > 10.0f && canTeleport &&
            (s_tickCount - s_lastGhostTeleportTick) >= kIdleTeleportCooldownTicks)
        {
            SetEntityCoords(s_ghost, p);
            cur = p;
            dist = 0.f;
            s_moveTargetValid = false;
            s_ghostFarTicks = 0;
            s_idleChase = false;
            s_lastGhostTeleportTick = s_tickCount;
            if (s_tickCount - s_lastGhostTpLogTick > 5 * 60)
            {
                s_lastGhostTpLogTick = s_tickCount;
                sh_log::Write("coop: ghost teleport (idle dist>10)\n");
            }
        }
        if (dist > 3.0f)
            ++s_ghostFarTicks;
        else
            s_ghostFarTicks = 0;

        const bool canMove = canTeleport;
        if (canMove && s_ghostFarTicks >= kGhostFarTicksForSprint && dist > 2.5f)
        {
            if (NeedGhostCatchupTask(p))
                IssueGhostFollowTask(s_ghost, p, 3.0f);
            s_idleChase = true;
            s_ghostFarTicks = 0;
        }
        else if (canMove && dist > 4.0f)
        {
            // Отстал на 4+ м — догоняем БЕГОМ, а не шагом (как раньше).
            if (NeedGhostCatchupTask(p))
                IssueGhostFollowTask(s_ghost, p, 2.0f);
            s_idleChase = true;
            s_ghostFarTicks = 0;
        }
        else
        {
            if (dist > RESUME)
                s_idleChase = true;
            else if (dist < CLOSE)
                s_idleChase = false;

            if (s_idleChase)
            {
                // Мягкий догон шагом с гистерезисом — чтобы не «танцевать»
                // на месте при покачивании корпуса рядом с целью.
                ApplyMoveToGhost(s_ghost,
                                 static_cast<uint8_t>(coop::MoveState::Walk), p);
            }
            else
            {
                // Стоит рядом с целью: не трогаем XY, только прижим при подвесе.
                // Порог поднят 1.0 -> 2.5 м: раз в секунду SetEntityCoords на
                // неровностях давал «дёрганье» стоящего призрака. В движении/
                // бою/верхом/в транспорте/сценарии не двигаем вообще.
                float groundZ = 0.f;
                const bool hasGround = GetGroundZForCoord(cur, groundZ);
                const bool fixAllowed =
                    !IsGhostMounted() && !s_remoteInVehicle &&
                    !s_combatActive && !GhostShotReplayActive() &&
                    s_remoteActionRaw != static_cast<uint8_t>(net::Action::InScenario) &&
                    s_remoteActionRaw != static_cast<uint8_t>(net::Action::Sitting) &&
                    s_remoteActionRaw != static_cast<uint8_t>(net::Action::Drunk);
                if (hasGround && fixAllowed &&
                    (cur.z - groundZ) > 2.5f && groundZ > -100.f &&
                    (s_tickCount - s_lastIdleGroundFixTick) > 60)
                {
                    // Опускаем на землю, если реально висим НАД ней >2.5 м.
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
    }

    // Поворот интерполируем мягко: кратчайшая дуга. МЁРТВАЯ ЗОНА 2°:
    // не переписываем heading мелкими коррекциями каждый кадр — именно это
    // давало микро-дрожь стоящего педа (idle-анимация слегка смещает корпус,
    // а мы 15 раз/с «поправляли» поворот на 25% → пед трясся в покое).
    // ВАЖНО: во время прицеливания/стрельбы поворот НЕ трогаем — heading
    // тела игрока может не совпадать с направлением камеры, и наш SetEntityHeading
    // каждый кадр разворачивал педа «строго вперёд», перебивая прицел
    // (задачу TASK_AIM_GUN_AT_COORD). Ориентацией управляет боевая задача.
    return false;
}

// ub-mode unit B4: heading + pedState/outfit tail ApplyRemoteFrame.
void ApplyRemoteFrameVisualTail(const net::FramePayload& f, float lerpFactor)
{
    if (!s_combatActive && !GhostShotReplayActive())
    {
        float h = f.heading - GetEntityHeading(s_ghost);
        while (h > 180.f) h -= 360.f;
        while (h < -180.f) h += 360.f;
        // Мёртвая зона 5°: мелкие колебания heading отправителя (стоя) не
        // должны каждый кадр доворачивать педа — это микро-дрожь в покое.
        if (std::fabs(h) > 5.0f)
            SetEntityHeading(s_ghost, GetEntityHeading(s_ghost) + h * lerpFactor);
    }

    // Применить особое состояние (ragdoll/cover/swim), оружие (при смене).
    ApplyPedStateToGhost(f);

    // Применить мета-наряд удалённого игрока (внешний вид) — при смене хэша.
    ApplyPlayerOutfitToGhost(f.reserved[1]);
}

void ApplyRemoteFrame(const net::FramePayload& f)
{
    if (ApplyRemoteFrameEarly(f))
        return;

    constexpr float LERP_FACTOR = 0.2f; // ~20% за тик при 60 Гц (плавно, без рывков)
    constexpr float TELEPORT_DIST = 10.0f; // больше этой дистанции — телепорт

    Vec3 cur = GetEntityCoords(s_ghost);
    Vec3 p{f.x, f.y, f.z};

    const uint8_t move = static_cast<uint8_t>(f.move);
    const uint8_t rideState = static_cast<uint8_t>(coop::MoveState::RideHorse);
    // Отсев мерцания: IS_PED_ON_MOUNT у отправителя может кратко дрожать,
    // поэтому «не верхом» считаем только после kRideLossTicks подряд.
    if (ApplyRemoteFrameMountPhase(f, p, move, rideState))
        return;

    if (ApplyRemoteFramePositionPhase(f, cur, p, move, TELEPORT_DIST))
        return;

    ApplyRemoteFrameVisualTail(f, LERP_FACTOR);
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
    s_ghostNeedsBaseScrub = true;
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
    // Оружие: drawn = в руках (не в кобуре), changing = идёт анимация
    // доставания/убирания. Нужны приёмнику для draw/holster-анимаций.
    Init(natives::_IS_PED_CURRENT_WEAPON_HOLSTERED); Push(ped);
    if (!Call()) f |= net::kFlagWeaponDrawn;
    Init(natives::_IS_WEAPON_HOLSTER_STATE_CHANGING); Push(ped);
    if (Call()) f |= net::kFlagWeaponChanging;
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
// Пока для текущего оружия ammo-детект работает, impact/latch-fallback
// ВЫКЛЮЧЕН (иначе дубли/потери); fallback остаётся для лука/метательного,
// где натив не отдаёт магазин.
Vec3 s_lastImpact{0.f, 0.f, 0.f};
bool s_localWasShooting = false;
int  s_lastClipAmmo = -1;
bool s_ammoQueryFailLogged = false;
// Текущее оружие ammo-детекта и работает ли для него натив.
unsigned long long s_ammoDetectWeapon = 0;
bool s_ammoDetectActive = false;
// Выстрелы, определённые по патронам, но ещё не отправленные: не более
// kMaxShotsPerTick за тик, остаток уходит в следующих тиках (дробь/пачки).
int s_pendingAmmoShots = 0;
constexpr int kMaxShotsPerTick = 4;       // burst: не фантомная пачка
constexpr int kMaxPendingAmmoShots = 8;   // cap очереди после refill/смерти
// Редкая диагностика: лог ammo-детекта по изменению N/раз в ~2 с и
// throttled-лог отправки (не спамим на каждый выстрел авто-оружия).
int s_lastAmmoLogCount = -1;
unsigned int s_lastAmmoLogTick = 0;
unsigned int s_lastShotSendLogTick = 0;
// Гибридный fallback: ammo-детект считаем рабочим только если он недавно
// реально ловил выстрелы. Иначе (лук/метательное/скриптовое оружие/трейнер:
// GET_AMMO_IN_CLIP вернул true, но магазин не меняется) разрешаем impact/latch.
unsigned int s_lastAmmoDetectTick = 0;
bool s_ammoDetectEver = false;

// Сброс sender-детектора выстрелов. Вызывается из ResetGhostCombatState
// (смерть/респавн/cleanup/model change/дисконнект): baseline магазина и
// очередь Shot не должны переживать смерть/refill, иначе улетают «фантомы».
void ResetLocalShotDetector()
{
    s_lastClipAmmo = -1;
    s_ammoDetectWeapon = 0;
    s_ammoDetectActive = false;
    s_pendingAmmoShots = 0;
    s_lastAmmoDetectTick = 0;
    s_ammoDetectEver = false;
    s_lastAmmoLogCount = -1;
    s_localWasShooting = false;
    // s_lastImpact НЕ сбрасываем: после death/respawn impact натива может
    // вернуть старую точку, и разница с нулём дала бы фантомный Shot.
    s_ammoQueryFailLogged = false;
}

// Реализация ниже (anonymous namespace), нужна здесь для детекта выстрела.
unsigned long long GetLocalWeaponHash(unsigned int ped);

// E5 (v17): урон по NPC-прокси от гостя — по классу оружия (30..80).
uint16_t NpcDamageForWeapon(unsigned long long weapon)
{
    if (!weapon) return 30;
    auto isW = [weapon](unsigned long long nativeHash) -> bool {
        using namespace natives_api;
        Init(nativeHash);
        Push(static_cast<unsigned long long>(weapon));
        return Call() != 0;
    };
    if (isW(natives::IS_WEAPON_SHOTGUN)) return 80;
    if (isW(natives::IS_WEAPON_RIFLE) || isW(natives::IS_WEAPON_REPEATER) ||
        isW(natives::_IS_WEAPON_SNIPER)) return 60;
    if (isW(natives::IS_WEAPON_REVOLVER) || isW(natives::IS_WEAPON_PISTOL))
        return 35;
    return 30;
}

// E5.1: хедшот — проверяем, проходит ли выстрел/луч камеры через сферу
// головы (кость SKEL_Head, радиус ~0.30 м), и голова ближе центра тела.
// Если кость/камера недоступны — false (graceful fallback на body-урон).
bool IsEntityHeadshot(unsigned int entity, const Vec3* impact)
{
    using namespace natives_api;
    static const char kHeadBoneName[] = "SKEL_Head";
    Init(natives::GET_ENTITY_BONE_INDEX_BY_NAME);
    Push(entity);
    Push(reinterpret_cast<unsigned long long>(kHeadBoneName));
    const int bone = static_cast<int>(Call());
    if (bone <= 0)
        return false;
    Vec3 head{0.f, 0.f, 0.f};
    Init(natives::GET_WORLD_POSITION_OF_ENTITY_BONE);
    Push(entity);
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(bone)));
    CallVec3(head);
    if (!std::isfinite(head.x) || !std::isfinite(head.y) ||
        !std::isfinite(head.z))
        return false;
    if (head.x == 0.f && head.y == 0.f && head.z == 0.f)
        return false;
    const Vec3 body = GetEntityCoords(entity);
    auto dist2 = [](const Vec3& p, const Vec3& q) {
        const float dx = p.x - q.x;
        const float dy = p.y - q.y;
        const float dz = p.z - q.z;
        return dx * dx + dy * dy + dz * dz;
    };
    constexpr float kHeadRadius2 = 0.30f * 0.30f;

    // 1) Точка попадания последнего выстрела (точнее, если натив доступен).
    if (impact && (impact->x != 0.f || impact->y != 0.f || impact->z != 0.f))
    {
        const float dh = dist2(*impact, head);
        return dh <= kHeadRadius2 && dh < dist2(*impact, body);
    }

    // 2) Fallback: луч камеры (GET_GAMEPLAY_CAM_COORD + GET_GAMEPLAY_CAM_ROT).
    Vec3 cam{0.f, 0.f, 0.f};
    Init(natives::GET_GAMEPLAY_CAM_COORD);
    CallVec3(cam);
    Vec3 rot{0.f, 0.f, 0.f};
    Init(natives::GET_GAMEPLAY_CAM_ROT);
    Push(static_cast<unsigned long long>(2)); // порядок как в SDK
    CallVec3(rot);
    constexpr float kDegToRad = 0.01745329252f;
    const float pitch = rot.x * kDegToRad;
    const float yaw = rot.z * kDegToRad;
    const float cp = std::cos(pitch);
    const Vec3 dir{-std::sin(yaw) * cp, std::cos(yaw) * cp, std::sin(pitch)};
    const Vec3 vh{head.x - cam.x, head.y - cam.y, head.z - cam.z};
    const float t = vh.x * dir.x + vh.y * dir.y + vh.z * dir.z;
    if (t <= 0.f)
        return false;
    const Vec3 proj{cam.x + dir.x * t, cam.y + dir.y * t, cam.z + dir.z * t};
    if (dist2(proj, head) > kHeadRadius2)
        return false;
    const Vec3 vb{body.x - cam.x, body.y - cam.y, body.z - cam.z};
    const float tb = vb.x * dir.x + vb.y * dir.y + vb.z * dir.z;
    return tb <= 0.f || t < tb;
}

// E5 (v17): после реального выстрела игрока — если free-aim цель является
// NPC-прокси, попросить хост применить урон (сам гость урон НЕ наносит).
// E5.1: headshot -> damage=250 (ваншот), иначе урон по классу оружия.
void TrySendNpcDamage(unsigned int localPed, unsigned long long weapon,
                      const Vec3* impact)
{
    if (!coopcfg::Get().npcActor || !coopcfg::Get().npcDamage)
        return;
    if (s_amHost.load())
        return; // E5: урон применяет хост, запросы шлёт только гость
    using namespace natives_api;
    Init(natives::GET_PLAYER_INDEX);
    const int player = static_cast<int>(Call());
    if (player < 0) return;
    unsigned long long entityBuf = 0;
    Init(natives::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT);
    Push(static_cast<unsigned long long>(player));
    Push(reinterpret_cast<unsigned long long>(&entityBuf));
    if (!Call()) return;
    const unsigned int target = static_cast<unsigned int>(entityBuf);
    if (!target) return;
    const Vec3 a = GetEntityCoords(localPed);
    const Vec3 b = GetEntityCoords(target);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    if (dx * dx + dy * dy + dz * dz > 120.0f * 120.0f)
        return;
    uint32_t actorId = 0;
    if (!actor_sync::GetDamageTargetInfo(target, actorId))
        return;
    const bool headshot = IsEntityHeadshot(target, impact);
    const uint16_t dmg =
        headshot ? static_cast<uint16_t>(250) : NpcDamageForWeapon(weapon);
    actor_sync::SendDamageToActor(target, dmg, static_cast<uint32_t>(weapon),
                                  headshot);
}

// ub-mode unit C2-1: независимые шаги детектора выстрелов.
void HandleLocalWeaponChange(unsigned long long weapon)
{
    if (weapon == s_ammoDetectWeapon)
        return;
    s_ammoDetectWeapon = weapon;
    s_lastClipAmmo = -1;
    s_ammoDetectActive = false;
    s_ammoDetectEver = false;
    s_pendingAmmoShots = 0;
}

bool QueryLocalClipAmmo(unsigned int ped, unsigned long long weapon,
                        int& clipAmmo)
{
    using namespace natives_api;
    if (!weapon)
        return false;
    Init(natives::GET_AMMO_IN_CLIP);
    Push(ped);
    Push(reinterpret_cast<unsigned long long>(&clipAmmo));
    Push(weapon);
    const bool haveAmmo = Call() != 0;
    if (!haveAmmo && !s_ammoQueryFailLogged)
    {
        s_ammoQueryFailLogged = true;
        sh_log::Write("coop: GET_AMMO_IN_CLIP failed for weapon 0x%llX; "
                      "using impact/shooting fallback\n", weapon);
    }
    return haveAmmo;
}

void UpdateLocalAmmoDetector(bool haveAmmo, int clipAmmo, bool reloading,
                             unsigned long long weapon)
{
    if (haveAmmo)
    {
        // Смена оружия / перезарядка / refill / первый замер — это НЕ выстрелы:
        // переустанавливаем базовый счётчик и выбрасываем неотправленную
        // очередь (иначе после скриптового refill уйдут «фантомы»).
        if (reloading || s_lastClipAmmo < 0 || clipAmmo > s_lastClipAmmo)
        {
            s_lastClipAmmo = clipAmmo;
            s_pendingAmmoShots = 0;
        }
        else if (clipAmmo < s_lastClipAmmo)
        {
            const int n = s_lastClipAmmo - clipAmmo;
            s_lastClipAmmo = clipAmmo;
            s_lastAmmoDetectTick = s_tickCount;
            s_ammoDetectEver = true;
            s_pendingAmmoShots = std::min(s_pendingAmmoShots + n,
                                          kMaxPendingAmmoShots);
            // Диагностика: по изменению N или раз в ~2 с (не спамим).
            if (n != s_lastAmmoLogCount ||
                s_tickCount - s_lastAmmoLogTick > 2 * 60)
            {
                s_lastAmmoLogCount = n;
                s_lastAmmoLogTick = s_tickCount;
                sh_log::Write("coop: ammo detect %d shots (weapon 0x%llX, "
                              "pending %d)\n", n, weapon, s_pendingAmmoShots);
            }
        }
    }
}

void UpdateLocalShotEvents(unsigned int ped)
{
    using namespace natives_api;

    // Локальный игрок мёртв/умирает: магазин может читаться как 0/refill —
    // это НЕ выстрелы. Сбрасываем baseline+очередь, чтобы не отправить
    // фантомные Shot пачкой (гость стреляет реальными пулями).
    if (IsPedDeadOrDying(ped))
    {
        ResetLocalShotDetector();
        return;
    }

    // 0) Смена оружия: ammo-латч и неотправленный остаток старого оружия
    // неактуальны (точки/счётчики от прошлого ствола не шлём).
    const unsigned long long weapon = GetLocalWeaponHash(ped);
    HandleLocalWeaponChange(weapon);

    // 1) Ammo-детект: каждый тик читаем патроны в магазине.
    int clipAmmo = 0;
    const bool haveAmmo = QueryLocalClipAmmo(ped, weapon, clipAmmo);
    s_ammoDetectActive = haveAmmo;

    bool reloading = false;
    if (weapon)
    {
        Init(natives::IS_PED_RELOADING); Push(ped);
        reloading = Call() != 0;
    }
    UpdateLocalAmmoDetector(haveAmmo, clipAmmo, reloading, weapon);

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

    // 3) Гибридный fallback (impact/latch). Ammo-детект «глушит» fallback
    // только если он реально ловил выстрелы недавно (~30 тиков). Для лука/
    // метательного/скриптового оружия (натив вернул true, но магазин не
    // меняется) fallback работает — иначе выстрелов не было вообще.
    Init(natives::IS_PED_SHOOTING); Push(ped);
    const bool shooting = Call() != 0;
    const bool ammoDetectFresh = s_ammoDetectActive && s_ammoDetectEver &&
        (s_tickCount - s_lastAmmoDetectTick) <= 30;
    bool fallback = false;
    if (!ammoDetectFresh)
    {
        if (haveImpact)
        {
            if (impact.x != s_lastImpact.x || impact.y != s_lastImpact.y ||
                impact.z != s_lastImpact.z)
            {
                s_lastImpact = impact;
                fallback = true;
            }
        }
        if (!fallback)
        {
            if (shooting && !s_localWasShooting)
                fallback = true;
            else if (!haveAmmo && shooting &&
                     s_tickCount - s_lastShotEventTick >= 8)
                fallback = true;
        }
        // Fallback не чаще ~30 Гц.
        if (fallback && s_tickCount - s_lastShotEventTick < 2)
            fallback = false;
    }
    else if (haveImpact)
    {
        s_lastImpact = impact; // тихо, без отправки
    }
    s_localWasShooting = shooting;

    const Vec3 sendPoint = haveImpact
        ? impact
        : Vec3{s_localAimX, s_localAimY, s_localAimZ};

    // «Фантомы» после death/refill: очередь должна жить 1-2 тика. Если
    // ammo-детект давно не срабатывал и пед не стреляет — сбрасываем.
    if (s_pendingAmmoShots > 0 && !shooting &&
        (s_tickCount - s_lastAmmoDetectTick) > 5)
        s_pendingAmmoShots = 0;

    // 4) Отправка: ammo-очередь не более kMaxShotsPerTick за тик (остаток —
    // в следующих тиках), fallback — не более 1. Очередь пополняется только
    // реальным уменьшением ammo, поэтому после death/refill она очищена
    // (ResetLocalShotDetector), а мёртвый пед сюда уже не дошёл.
    int sentAmmo = 0;
    while (s_pendingAmmoShots > 0 && sentAmmo < kMaxShotsPerTick)
    {
        net::ShotPayload sp{};
        sp.x = sendPoint.x; sp.y = sendPoint.y; sp.z = sendPoint.z;
        g_netClient.SendShot(sp);
        --s_pendingAmmoShots;
        s_lastShotEventTick = s_tickCount;
        ++sentAmmo;
    }
    // Дедуп по тику: если в этом тике уже ушли ammo-Shot (или pending ещё
    // не разобран), fallback не отправляем — иначе дубли.
    if (fallback && (sentAmmo > 0 || s_pendingAmmoShots > 0))
        fallback = false;
    if (sentAmmo > 0 && s_tickCount - s_lastShotSendLogTick >= 60)
    {
        // Не чаще раза в секунду: иначе auto-огонь заливает лог.
        s_lastShotSendLogTick = s_tickCount;
        sh_log::Write("coop: send shot (%.1f %.1f %.1f) [ammo x%d, pending %d]\n",
                      sendPoint.x, sendPoint.y, sendPoint.z, sentAmmo,
                      s_pendingAmmoShots);
    }
    if (fallback)
    {
        net::ShotPayload sp{};
        sp.x = sendPoint.x; sp.y = sendPoint.y; sp.z = sendPoint.z;
        g_netClient.SendShot(sp);
        s_lastShotEventTick = s_tickCount;
        sh_log::Write("coop: send shot (%.1f %.1f %.1f) [fallback]\n",
                      sp.x, sp.y, sp.z);
    }

    // E5 (v17): реальный выстрел в NPC-прокси -> запрос урона хосту.
    // E5.1: передаём точку попадания для headshot-детекта.
    if (sentAmmo > 0 || fallback)
        TrySendNpcDamage(ped, weapon, haveImpact ? &impact : nullptr);
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
    if (s_combatActive || GhostShotReplayActive())
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

// Стабильный ВЫБРАННЫЙ хэш оружия игрока (для синхронизации draw/holster):
// GET_CURRENT_PED_WEAPON с attachPoint=0 возвращает 0, когда оружие в кобуре,
// поэтому для флагов drawn/changing нужен _GET_PED_CURRENT_HELD_WEAPON.
unsigned long long GetLocalSelectedWeaponHash(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_GET_PED_CURRENT_HELD_WEAPON);
    Push(ped);
    unsigned long long out = static_cast<unsigned long long>(Call());
    // Натив может вернуть enum/не-hash — тогда fallback на GET_CURRENT_PED_WEAPON.
    if (!out || !IsWeaponHashValid(out))
    {
        // Fallback: натив недоступен/вернул 0/не-hash — берём оружие в руке
        // (при стрельбе оно не в кобуре). Иначе сломалась бы синхронизация
        // оружия и выстрелы у призрака.
        unsigned long long cur = 0;
        Init(natives::GET_CURRENT_PED_WEAPON);
        Push(ped);
        Push(reinterpret_cast<unsigned long long>(&cur));
        Push(static_cast<unsigned long long>(1));
        Push(static_cast<unsigned long long>(0)); // attachPoint = HAND_PRIMARY
        Push(static_cast<unsigned long long>(0));
        Call();
        out = cur;
    }
    if (out == natives::WEAPON_UNARMED ||
        (out && !IsWeaponHashValid(out)))
        out = 0;
    return out;
}

// Обработка входящего кадра: модель персонажа удалённого игрока (reserved[2]).
// Реальная модель отличается от модели текущего педа → пересоздаём педа
// (удаляем старый, новый заспавнится в Tick по wantModel), но только при
// РЕАЛЬНОМ изменении и не чаще kModelRecreateCooldownTicks. Fallback-модель из
// PLAYER_JOINED сюда не попадает — она хранится отдельно (s_joinedModel),
// поэтому join/reconnect больше не вызывает пересоздания. Модель 0 не трогаем.
void CheckRemoteModelChange(const net::FramePayload& f)
{
    const unsigned long long realModel = f.reserved[2];
    if (!realModel) return;
    // Модель пришла из сети: невалидный хэш НЕ должен приводить к удалению
    // текущего педа и RequestModel (иначе HasModelLoaded()==false навсегда и
    // Tick перестаёт обрабатывать сеть). Невалидную модель просто игнорируем.
    if (!IsModelHashValid(realModel))
    {
        if (realModel != s_ignoredPedModelHash)
        {
            s_ignoredPedModelHash = realModel;
            sh_log::Write("coop: ignoring invalid remote model 0x%llX\n",
                          realModel);
        }
        return;
    }
    s_ignoredPedModelHash = 0;
    // Первая реальная модель персонажа: запоминаем её. После этого fallback из
    // PLAYER_JOINED не перезаписывает модель (см. обработчик PlayerJoined), а
    // спавн после reconnect сразу идёт по реальной модели.
    const bool firstReal = !s_realModelSeen;
    s_realModelSeen = true;
    s_lastRealModel = realModel;
    if (realModel == s_ghostModel)
    {
        s_requestedModel = realModel; // пед уже с этой моделью
        return;
    }
    if (!s_ghost || !DoesEntityExist(s_ghost))
    {
        // Педа ещё нет — удалять нечего: просто просим правильную модель,
        // спавн ниже использует s_requestedModel/s_lastRealModel.
        s_requestedModel = realModel;
        RequestModel(realModel);
        s_modelLoadedLogged = false;
        if (firstReal)
            sh_log::Write("coop: real remote model 0x%llX known\n", realModel);
        return;
    }
    // Кулдаун пересоздания: защита от серии событий/дёрганья (не чаще ~1.5 с).
    if (s_lastGhostRecreateTick != 0 &&
        (s_tickCount - s_lastGhostRecreateTick) < kModelRecreateCooldownTicks)
    {
        sh_log::Write("coop: remote model change 0x%llX -> 0x%llX deferred "
                      "(recreate cooldown)\n", s_ghostModel, realModel);
        return;
    }
    sh_log::Write("coop: remote model changed 0x%llX -> 0x%llX "
                  "(real model, recreate)\n", s_ghostModel, realModel);
    s_lastGhostRecreateTick = s_tickCount;
    // Запоминаем новую модель и просим её загрузить (в Tick чуть ниже будет
    // использована при спавне). Старого педа удаляем — новый появится сразу.
    // Позицию для респавна берём АКТУАЛЬНУЮ (последний кадр/known pos), а не
    // позицию входа в сессию — иначе пед «прыгает» назад при пересоздании.
    if (s_lastKnownPosValid)
    {
        s_pendingHasPos = true;
        s_pendingX = s_lastKnownPos.x;
        s_pendingY = s_lastKnownPos.y;
        s_pendingZ = s_lastKnownPos.z;
        s_pendingHeading = s_lastKnownHeading;
    }
    s_requestedModel = realModel;
    RequestModel(realModel);
    s_modelLoadedLogged = false;
    s_ghostModel = realModel;
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
    s_ghostNeedsBaseScrub = true;
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
// ub-mode unit C1: фазы ApplyRemoteActionV2 (порядок сохранён).
void FinishPreviousRemoteAction(uint8_t mode, uint8_t scenarioMode,
                               uint8_t coverMode, uint8_t carryMode,
                               uint8_t lootMode)
{
    using namespace natives_api;
    const bool wasScenario = (s_appliedActionMode == scenarioMode);
    const bool wasCover = (s_appliedActionMode == coverMode);
    const bool wasCarryOrLoot =
        (s_appliedActionMode == carryMode || s_appliedActionMode == lootMode);
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

}

void ApplyRemoteScenarioAction(const net::ActionStatePayload& a, uint8_t mode,
                               uint8_t scenarioMode)
{
    using namespace natives_api;
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

}

void ApplyRemoteCoverAction(uint8_t mode, uint8_t coverMode, bool wasCover)
{
    using namespace natives_api;
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

}

void ApplyRemoteCarryLootAction(const net::ActionStatePayload& a, uint8_t mode,
                                uint8_t carryMode, uint8_t lootMode)
{
    using namespace natives_api;
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

}

void ApplyRemoteActionV2(const net::ActionStatePayload& a)
{
    if (!s_ghost || !DoesEntityExist(s_ghost) || s_ghostDead)
        return;
    const uint8_t mode = a.mode;
    const uint8_t scenarioMode = static_cast<uint8_t>(net::ActionMode::Scenario);
    const uint8_t coverMode    = static_cast<uint8_t>(net::ActionMode::Cover);
    const uint8_t carryMode    = static_cast<uint8_t>(net::ActionMode::Carry);
    const uint8_t lootMode     = static_cast<uint8_t>(net::ActionMode::Loot);
    FinishPreviousRemoteAction(mode, scenarioMode, coverMode, carryMode,
                               lootMode);
    // P4: wasCover фиксируем ПОСЛЕ Finish; инвариант — Finish не меняет
    // s_appliedActionMode (cover-фаза применяет уже вычисленный mode).
    const bool wasCover = (s_appliedActionMode == coverMode);
    ApplyRemoteScenarioAction(a, mode, scenarioMode);
    ApplyRemoteCoverAction(mode, coverMode, wasCover);
    ApplyRemoteCarryLootAction(a, mode, carryMode, lootMode);

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
    // task-157: клиентский traffic-счётчик (perf: net tx/rx) — в rdr2-coop.log,
    // а не в stderr. Колбэк вызывается сетевым потоком; sh_log потокобезопасен.
    g_netClient.SetTrafficLog([](const std::string& s) {
        sh_log::Write("%s\n", s.c_str());
    });
    // task-160: один раз предупредить об опасных комбинациях INI.
    coopcfg::WarnDangerousCombos();
    // E6 (v18): локальная эпоха presence (nonzero; используется как маркер).
    s_presenceEpoch = (static_cast<uint32_t>(GetTickCount64()) &
                       0x7FFFFFFFu) | 1u;
    s_presenceSentValid = false;
    s_localPresenceFlags = 0;
    s_lastSentPresenceFlags = 0;
    s_remotePresenceValid = false;
    s_remotePresenceFlags = 0;

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
        const unsigned long long now = GetTickCount64();
        if (s_shotQCount < kShotQueueSize)
        {
            s_shotQueue[s_shotQCount] = Vec3{sp.x, sp.y, sp.z};
            s_shotQueueMs[s_shotQCount] = now;
            ++s_shotQCount;
            s_shotEventsPending.store(true);
        }
        else
        {
            // Переполнение: выбрасываем САМОЕ СТАРОЕ событие, чтобы призрак
            // проигрывал актуальные выстрелы (визуальная аппроксимация).
            for (int i = 1; i < s_shotQCount; ++i)
            {
                s_shotQueue[i - 1] = s_shotQueue[i];
                s_shotQueueMs[i - 1] = s_shotQueueMs[i];
            }
            s_shotQueue[s_shotQCount - 1] = Vec3{sp.x, sp.y, sp.z};
            s_shotQueueMs[s_shotQCount - 1] = now;
            s_shotDropped.fetch_add(1);
        }
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

    // E1 (v15): actor-репликация NPC. Колбэки только копируют данные в inbox
    // actor_sync (без нативов/логов); игровой слой применит их в Tick.
    g_netClient.SetActorSpawnBatch(actor_sync::OnActorSpawnBatch);
    g_netClient.SetActorFrames(actor_sync::OnActorFrames);
    g_netClient.SetActorDespawn(actor_sync::OnActorDespawn);
    g_netClient.SetActorEvent(actor_sync::OnActorEvent);
    g_netClient.SetActorOutfit(actor_sync::OnActorOutfit);
    g_netClient.SetActorResyncRequest(actor_sync::OnActorResyncRequest);
    // E2c-b (v16): точная внешность NPC (asset/tint) от хоста — в inbox.
    g_netClient.SetActorAppearance(actor_sync::OnActorAppearance);
    // E5 (v17): запрос урона от гостя (0x1F) — в inbox actor_sync.
    g_netClient.SetActorDamageRequest(actor_sync::OnActorDamageRequest);

    // E3d-c (v19): occupants транспорта от хоста -> world_sync (копия).
    g_netClient.SetVehicleOccupants(
        [](uint32_t vehicleNetId,
           const std::vector<net::VehicleOccupantEntry>& entries) {
            world_sync::OnVehicleOccupants(vehicleNetId, entries);
        });
    // E3d-b (v19): поезд/состав от хоста -> world_sync (копия под mutex).
    g_netClient.SetTrainSnapshotV2(
        [](const net::TrainSnapshotV2Payload& payload) {
            world_sync::OnTrainSnapshotV2(payload);
        });

    // E6 (v18): UI-статус другого игрока (карта/пауза/сатчель/магазин).
    g_netClient.SetPlayerPresence([](uint32_t epoch, uint16_t flags) {
        std::lock_guard<std::mutex> lk(s_inboxMtx);
        InboxEvents& ev = s_inbox.events;
        ev.presenceValid = true;
        ev.presenceEpoch = epoch;
        ev.presenceFlags = flags;
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

// E3d-c (v19): occupants транспорта от хоста (0x21).
void SendVehicleOccupants(
    uint32_t vehicleNetId,
    const std::vector<net::VehicleOccupantEntry>& entries)
{
    g_netClient.SendVehicleOccupants(vehicleNetId, entries);
}

// E3d-b (v19): снапшот поезда/состава (0x22).
void SendTrainSnapshotV2(const net::TrainSnapshotV2Payload& payload)
{
    g_netClient.SendTrainSnapshotV2(payload);
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

// E1: отправка actor-сообщений от хоста (вызывается actor_sync::Tick).
void SendActorSpawnBatch(uint32_t epoch,
                         const std::vector<net::ActorSpawn>& actors)
{
    g_netClient.SendActorSpawnBatch(epoch, actors);
}

// E2c-b (v16): точная внешность NPC (asset/tint) — хост релеит гостям.
void SendActorAppearance(uint32_t epoch, uint32_t actorId,
                         const std::vector<net::ActorAppearanceEntry>& entries)
{
    g_netClient.SendActorAppearance(epoch, actorId, entries);
}

// E9 (task-142): horse tack (0x1C) и сбор outfit любого педа (horse).
void SendActorOutfit(uint32_t epoch, uint32_t actorId,
                     const net::OutfitState& outfit)
{
    g_netClient.SendActorOutfit(epoch, actorId, outfit);
}

void CollectLocalOutfitForActor(unsigned int ped, net::OutfitState& out)
{
    CollectLocalOutfit(ped, out);
}

// E5 (v17): гость -> хост запрос урона по NPC (0x1F).
bool SendActorDamage(uint32_t epoch, uint32_t actorId, uint16_t damage,
                     uint32_t weaponHash)
{
    return g_netClient.SendActorDamage(epoch, actorId, damage, weaponHash);
}

// E6 (v18): сообщить свой UI-статус другому игроку.
bool SendPlayerPresence(uint32_t epoch, uint16_t flags)
{
    return g_netClient.SendPlayerPresence(epoch, flags);
}

void SendActorFrames(uint32_t epoch, uint16_t seq,
                     const std::vector<net::ActorFrame>& frames)
{
    g_netClient.SendActorFrames(epoch, seq, frames);
}

void SendActorDespawn(const net::ActorDespawnPayload& msg)
{
    g_netClient.SendActorDespawn(msg);
}

void SendActorResync(uint32_t epoch, uint32_t lastSeq, uint8_t reason)
{
    // Гость просит хост прислать ростер/кадры заново. Троттлинг: не чаще
    // одного запроса в секунду (несколько прокси могут одновременно
    // обнаружить потерю кадров и запросить resync).
    if (epoch == 0)
        return;
    static unsigned int s_lastResyncTick = 0;
    if (s_lastResyncTick != 0 && (s_tickCount - s_lastResyncTick) < 60)
        return;
    s_lastResyncTick = s_tickCount;
    g_netClient.SendActorResync(epoch, lastSeq, reason);
    sh_log::Write("actor: resync request epoch=%u seq=%u reason=%u\n",
                  epoch, lastSeq, (unsigned)reason);
}

// ub-mode unit A1: выделенные фазы coop::Tick (без переупорядочивания).
void TickWorldStateSync()
{
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
}

void TickJoinSubtitle()
{
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

}

void TickRttHud()
{
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

void TickPersistGhostPosition()
{
    if (s_ghost && s_pedName[0] && DoesEntityExist(s_ghost) &&
        (s_tickCount % (5 * 60) == 1))
    {
        Vec3 gp = GetEntityCoords(s_ghost);
        float hd = GetEntityHeading(s_ghost);
        persist::SaveLastPosition(s_pedName, gp.x, gp.y, gp.z, hd);
    }
}

// ub-mode unit A4: perf-окно полного кадра (вынесено из Tick).
void TickPerfWindow(unsigned long long perfFrameStart,
                    unsigned long long perfWorldUs,
                    unsigned long long perfActorUs)
{
    // P0-1 (task-156): полный кадр coop::Tick = world + actor + coop.
    {
        const unsigned long long frameUs = QpcDeltaUs(perfFrameStart);
        const unsigned long long coopUs =
            frameUs > perfWorldUs + perfActorUs
                ? frameUs - perfWorldUs - perfActorUs
                : 0;
        g_framePerf.Add(static_cast<double>(frameUs));
        g_worldPerf.Add(static_cast<double>(perfWorldUs));
        g_actorPerf.Add(static_cast<double>(perfActorUs));
        g_coopPerf.Add(static_cast<double>(coopUs));
        if (s_perfWindowStartMs == 0)
            s_perfWindowStartMs = GetTickCount64();
        if (++g_perfWindowTicks >= 300)
        {
            if (coopcfg::Get().npcDebug)
            {
                const unsigned long long nowMs = GetTickCount64();
                const unsigned long long elapsedMs =
                    nowMs > s_perfWindowStartMs
                        ? nowMs - s_perfWindowStartMs
                        : 5000ull;
                const double tickHz =
                    elapsedMs > 0
                        ? 300.0 * 1000.0 / static_cast<double>(elapsedMs)
                        : 60.0;
                sh_log::Write(
                    "perf: frame %s tick=%.2f p95=%.2f max=%.2f | "
                    "world=%.2f p95=%.2f | actor=%.2f p95=%.2f | "
                    "coop=%.2f p95=%.2f frames_hz=%.1f tick_hz=%.1f\n",
                    s_amHost.load() ? "host" : "guest",
                    g_framePerf.Avg() / 1000.0, g_framePerf.P95() / 1000.0,
                    g_framePerf.Max() / 1000.0,
                    g_worldPerf.Avg() / 1000.0, g_worldPerf.P95() / 1000.0,
                    g_actorPerf.Avg() / 1000.0, g_actorPerf.P95() / 1000.0,
                    g_coopPerf.Avg() / 1000.0, g_coopPerf.P95() / 1000.0,
                    g_frameSentWindow / (elapsedMs / 1000.0), tickHz);
            }
            g_framePerf.Reset();
            g_worldPerf.Reset();
            g_actorPerf.Reset();
            g_coopPerf.Reset();
            g_perfWindowTicks = 0;
            g_frameSentWindow = 0;
            s_perfWindowStartMs = GetTickCount64();
        }
    }
}

void DrainLocalInbox()
{
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
}

// ub-mode unit A5-b1: отправка Player Frame (P0 throttle + horse outfit).
void SendLocalPlayerFrame(unsigned int playerPed)
{
    // P0-2 (task-156): Player Frame throttle. Раньше 88B слались каждый тик
    // (5.6 КБ/с @60, 13.4 @144). Теперь: не чаще player_frame_hz (10..60);
    // change_only=1 -> при изменении сразу (в пределах cap), в покое heartbeat 5 Гц.
    if (g_netStarted)
    {
        Vec3 pc = GetEntityCoords(playerPed);
        const float heading = GetEntityHeading(playerPed);
        const uint8_t move = DetectLocalMoveState(playerPed);
        const uint8_t state = DetectLocalPedState(playerPed);
        const uint8_t flags = DetectLocalFlags(playerPed);
        const float health = GetLocalHealth01(playerPed);
        const uint32_t weapon = GetLocalSelectedWeaponHash(playerPed);
        float aimX = 0.f, aimY = 0.f, aimZ = 0.f;
        if (s_localAimValid)
        {
            aimX = s_localAimX;
            aimY = s_localAimY;
            aimZ = s_localAimZ;
        }
        uint32_t mountModel = 0;
        if (move == static_cast<uint8_t>(coop::MoveState::RideHorse))
        {
            using namespace natives_api;
            Init(natives::GET_MOUNT);
            Push(playerPed);
            const unsigned int m = static_cast<unsigned int>(Call());
            if (m)
            {
                Init(natives::GET_ENTITY_MODEL);
                Push(m);
                mountModel = static_cast<uint32_t>(Call());
            }
        }
        unsigned long long pedModel = 0;
        unsigned long long metaOutfit = 0;
        {
            using namespace natives_api;
            Init(natives::GET_ENTITY_MODEL);
            Push(playerPed);
            pedModel = Call();
            Init(natives::GET_PED_META_OUTFIT_HASH);
            Push(playerPed);
            metaOutfit = Call();
        }

        PlayerFrameSig sig{};
        sig.x = pc.x; sig.y = pc.y; sig.z = pc.z;
        sig.heading = heading;
        sig.aimX = aimX; sig.aimY = aimY; sig.aimZ = aimZ;
        sig.health = health;
        sig.move = move; sig.state = state; sig.flags = flags;
        sig.action = s_localActionStable;
        sig.weapon = weapon; sig.mountModel = mountModel;
        sig.pedModel = pedModel; sig.metaOutfit = metaOutfit;

        bool changed = !s_lastPlayerFrameSigValid;
        if (!changed)
        {
            const PlayerFrameSig& o = s_lastPlayerFrameSig;
            const float dx = pc.x - o.x, dy = pc.y - o.y, dz = pc.z - o.z;
            if (dx * dx + dy * dy + dz * dz > 0.0025f) changed = true; // >5 см
            float dh = heading - o.heading;
            while (dh > 180.f) dh -= 360.f;
            while (dh < -180.f) dh += 360.f;
            if (std::fabs(dh) > 1.0f) changed = true; // >1°
            if (std::fabs(health - o.health) > 0.01f) changed = true;
            if (move != o.move || state != o.state || flags != o.flags ||
                s_localActionStable != o.action || weapon != o.weapon ||
                mountModel != o.mountModel || pedModel != o.pedModel ||
                metaOutfit != o.metaOutfit)
                changed = true;
            if (s_localAimValid)
            {
                const float ax = aimX - o.aimX, ay = aimY - o.aimY,
                            az = aimZ - o.aimZ;
                if (ax * ax + ay * ay + az * az > 0.0025f) changed = true;
            }
        }

        int hz = coopcfg::Get().playerFrameHz;
        if (hz < 10) hz = 10;
        if (hz > 60) hz = 60;
        const unsigned long long frameNowMs = GetTickCount64();
        const unsigned long long minIntervalMs =
            static_cast<unsigned long long>(1000 / hz);
        const unsigned long long sinceMs =
            s_lastPlayerFrameSendMs == 0
                ? 999999ull
                : frameNowMs - s_lastPlayerFrameSendMs;
        const bool heartbeat = sinceMs >= 200ull; // 5 Гц wall-clock
        bool want = false;
        if (!s_lastPlayerFrameSigValid || s_lastPlayerFrameSendMs == 0)
            want = true;
        else if (!coopcfg::Get().playerFrameChangeOnly)
            want = sinceMs >= minIntervalMs;
        else
            want = heartbeat || (changed && sinceMs >= minIntervalMs);

        if (want)
        {
            net::FramePayload fp{};
            fp.x = pc.x; fp.y = pc.y; fp.z = pc.z;
            fp.heading = heading;
            fp.move = move;
            fp.state = state;
            fp.flags = flags;
            fp.action = s_localActionStable;
            fp.health = health;
            if (s_localAimValid)
            {
                fp.aimX = aimX;
                fp.aimY = aimY;
                fp.aimZ = aimZ;
                const float ax = fp.aimX - fp.x, ay = fp.aimY - fp.y,
                            az = fp.aimZ - fp.z;
                fp.aimDist = std::sqrt(ax * ax + ay * ay + az * az);
            }
            fp.weaponHash = weapon;
            fp.reserved[1] = metaOutfit;
            fp.reserved[2] = pedModel;
            if (s_tickCount % (5 * 60) == 1)
                sh_log::Write("coop: send pedLook, outfit=0x%llX model=0x%llX "
                              "move=%u state=%u\n",
                              fp.reserved[1], fp.reserved[2], fp.move, fp.state);
            // Лошадь игрока: outfit собираем не чаще 10 Гц (P1-3), не каждый тик.
            if (fp.move == static_cast<uint8_t>(coop::MoveState::RideHorse))
            {
                using namespace natives_api;
                Init(natives::GET_MOUNT); Push(playerPed);
                const unsigned int m = static_cast<unsigned int>(Call());
                if (m)
                {
                    fp.reserved[0] = mountModel;
                    if (s_lastHorseOutfitCollectMs == 0 ||
                        frameNowMs - s_lastHorseOutfitCollectMs >= 100ull)
                    {
                        s_lastHorseOutfitCollectMs = frameNowMs;
                        s_lastHorseOutfitCollectTick = s_tickCount;
                        net::OutfitState ho{};
                        CollectLocalOutfit(m, ho);
                        const bool ch = !s_sentHorseOutfitValid ||
                                        !OutfitsEqual(ho, s_sentHorseOutfit);
                        const bool hb =
                            (s_tickCount - s_lastHorseOutfitSendTick) >=
                            kOutfitHeartbeatTicks;
                        if (ho.count > 0 && (ch || hb))
                        {
                            s_sentHorseOutfit = ho;
                            s_sentHorseOutfitValid = true;
                            s_lastHorseOutfitSendTick = s_tickCount;
                            g_netClient.SendHorseOutfit(ho);
                            sh_log::Write("coop: send horse outfit (%u comp)%s\n",
                                          ho.count, ch ? "" : " [heartbeat]");
                        }
                    }
                }
                else if (s_tickCount % (3 * 60) == 1)
                {
                    sh_log::Write("coop: send RideHorse but GET_MOUNT=0\n");
                }
            }
            fp.seq = s_tickCount;
            g_netClient.SendFrame(fp);
            s_lastPlayerFrameSig = sig;
            s_lastPlayerFrameSigValid = true;
            s_lastPlayerFrameSendTick = s_tickCount;
            s_lastPlayerFrameSendMs = frameNowMs;
            ++g_frameSentWindow;
        }
    }
}

// ub-mode unit A5-b2: отправка outfit локального игрока (debounce+heartbeat).
void SyncLocalOutfit(unsigned int playerPed)
{
    // Синхронизация одежды: собираем компоненты наряда локального игрока и
    // отправляем при их смене (не каждый кадр). У сюжетных персонажей
    // GET_PED_META_OUTFIT_HASH = 0, поэтому используем список компонентов.
    // Смена отправляется только если набор продержался ~0.5 с: доставание
    // оружия (кобуры/патронташи) дёргает список и вызывает лишние применения.
    // Сравнение — order-insensitive (порядок категорий от движка нестабилен),
    // плюс heartbeat раз в ~5 с: иначе после реконнекта одежда не пересылается.
    const unsigned long long outfitNowMs = GetTickCount64();
    if (g_netStarted &&
        (s_lastPlayerOutfitCollectMs == 0 ||
         outfitNowMs - s_lastPlayerOutfitCollectMs >= 100ull))
    {
        s_lastPlayerOutfitCollectMs = outfitNowMs;
        s_lastPlayerOutfitCollectTick = s_tickCount;
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
            // именно дёргает оружие — кобура/патронташ/пояс). Wearable тоже
            // считаем изменением (открытая/закрытая куртка и т.п.).
            int diffCount = 0, firstIdx = -1;
            unsigned int oldV = 0, newV = 0;
            for (int i = 0; i < net::kMaxOutfitComponents; ++i)
            {
                if (os.components[i] != s_sentOutfit.components[i] ||
                    os.wearable[i] != s_sentOutfit.wearable[i])
                {
                    ++diffCount;
                    if (firstIdx < 0)
                    {
                        firstIdx = i;
                        oldV = os.components[i] != s_sentOutfit.components[i]
                            ? s_sentOutfit.components[i] : s_sentOutfit.wearable[i];
                        newV = os.components[i] != s_sentOutfit.components[i]
                            ? os.components[i] : os.wearable[i];
                    }
                }
            }
            s_lastOutfitSendTick = s_tickCount;
            s_haveCandidateOutfit = false;
            s_candidateOutfitTicks = 0;
            s_sentOutfit = os;
            s_sentOutfitValid = true;
            g_netClient.SendOutfit(os);
            // Лог: обычные смены — всегда; heartbeat — раз в ~60 с (сам
            // heartbeat оставляем ради ресинка после reconnect/потерь).
            if (!heartbeat ||
                (s_tickCount - s_lastOutfitHeartbeatLogTick) >=
                    kOutfitHeartbeatLogTicks)
            {
                if (heartbeat)
                    s_lastOutfitHeartbeatLogTick = s_tickCount;
                char comps[256] = "";
                {
                    int off = 0;
                    for (uint32_t i = 0; i < os.count && off < 220; ++i)
                        off += std::snprintf(comps + off, sizeof(comps) - off,
                                             "%s0x%X", i ? " " : "",
                                             os.components[i]);
                }
                sh_log::Write("coop: send outfit (%u comp, model 0x%llX, diff=%d "
                              "first[%d]=0x%X->0x%X)%s [%s]\n",
                              os.count, os.model, diffCount, firstIdx, oldV, newV,
                              heartbeat ? " [heartbeat]" : "", comps);
            }
        }
    }
}

// ub-mode A5-c1: Action v2 — наблюдение/отправка действий локального игрока.
void UpdateLocalActionSync(unsigned int playerPed)
{
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
}

// ub-mode A5-c2: локальный vehicle-sync (детект/приём/cleanup).
void UpdateLocalVehicleSync(unsigned int playerPed)
{
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
}

void Tick()
{
    ++s_tickCount;
    // P0-1 (task-156): полный кадр coop::Tick (world+actor+coop) с p95/max.
    const unsigned long long perfFrameStart = QpcNow();
    unsigned long long perfWorldUs = 0;
    unsigned long long perfActorUs = 0;
    // P1.1: hold выстрела уменьшаем каждый тик, а не только при приходе кадра
    // (иначе при паузе/лаге сети очередь выстрелов «замерзает»).
    if (s_shootHoldTicks > 0)
        --s_shootHoldTicks;
    if (s_deadGuardTicks > 0)
        --s_deadGuardTicks;

    unsigned int playerPed = GetPlayerPedFor(0);
    if (!playerPed)
    {
        // task-176: на загрузке/респавне playerPed может отсутствовать —
        // сервер видит game-thread только по presence. Пока сети запущена,
        // шлём flags=0 каждые ~2000 мс (обычная presence-логика не работает
        // без ped; при появлении ped снова берёт верх, дублей нет).
        if (g_netStarted)
        {
            const unsigned long long loadingNowMs = GetTickCount64();
            static unsigned long long s_lastLoadingPresenceMs = 0;
            if (s_lastLoadingPresenceMs == 0 ||
                loadingNowMs - s_lastLoadingPresenceMs >= 2000ull)
            {
                s_lastLoadingPresenceMs = loadingNowMs;
                if (SendPlayerPresence(s_presenceEpoch, 0))
                    s_lastPresenceSendMs = loadingNowMs;
            }
        }
        return;
    }

    // Забрать из сетевого inbox последние кадр/наряд/состояние мира. Дальше
    // работаем только с игровыми копиями — гонок с сетевым потоком нет.
    DrainLocalInbox();

    // Spike 1 (модель A): диагностика мира — пулы сущностей, NPC/транспорт
    // рядом, mission-entity, FPS. Только чтение, геймплей не меняет.
    // P1-8 (task-156): world_scan (pools 2048) — только диагностика npc_debug
    // либо раз в 5 с, чтобы не давать спайк вне метрик.
    {
        const unsigned long long scanNowMs = GetTickCount64();
        if (coopcfg::Get().npcDebug ||
            s_lastWorldScanMs == 0 ||
            scanNowMs - s_lastWorldScanMs >= 5000ull)
        {
            s_lastWorldScanMs = scanNowMs;
            world_scan::Tick(playerPed, s_tickCount);
        }
    }

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
        // E6 (v18): детект/отправка локального UI-статуса.
        UpdateLocalPresence();
        // E7.1: гость подавляет локальную ambient-популяцию (visitor mode).
        UpdateVisitorMode();

        // P1: репликация мира/NPC. Хост шлёт снапшоты, гость применяет их
        // и подавляет собственную ambient-популяцию. По умолчанию выключено
        // в ini (world_sync = 1 на ОБОИХ ПК для теста).
        // E3d-a: world_sync=0 + npc_actor=1 + world_vehicles=1 — включаем
        // vehicle-only режим (полный world_sync, если включён, приоритетнее).
        const bool worldVehiclesMode =
            !coopcfg::Get().worldSync && coopcfg::Get().npcActor &&
            coopcfg::Get().worldVehicles;
        const bool worldSyncEnabled =
            coopcfg::Get().worldSync || worldVehiclesMode;
        float partnerX = 0.f, partnerY = 0.f, partnerZ = 0.f;
        bool partnerValid = false;
        if (s_ghost && DoesEntityExist(s_ghost))
        {
            const Vec3 gp = GetEntityCoords(s_ghost);
            partnerX = gp.x;
            partnerY = gp.y;
            partnerZ = gp.z;
            partnerValid = true;
        }
        {
            const unsigned long long start = QpcNow();
            world_sync::Tick(playerPed, s_tickCount, s_amHost.load(),
                             worldSyncEnabled,
                             coopcfg::Get().worldTrains && !worldVehiclesMode,
                             worldVehiclesMode,
                             coopcfg::Get().npcVehicleRadius,
                             partnerX, partnerY, partnerZ, partnerValid);
            perfWorldUs = QpcDeltaUs(start);
        }

        // P2: Action v2 — сценарии/сидение/курение/укрытие.
        UpdateLocalActionSync(playerPed);

    // P2: посадка в транспорт удалённого игрока.
        UpdateLocalVehicleSync(playerPed);

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
    // Приоритет модели: кадровая (уже запрошенная) → последняя РЕАЛЬНАЯ
    // модель (переживает reconnect: спавним сразу правильную, без пересоздания)
    // → fallback из PLAYER_JOINED → дефолт из конфига.
    unsigned long long wantModel = s_requestedModel ? s_requestedModel
                                  : ((s_realModelSeen && s_lastRealModel)
                                        ? s_lastRealModel
                                        : (s_remoteModel ? s_remoteModel
                                                         : GHOST_PED_MODEL));
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
        s_joinOkSeen = true;
        s_amHost.store(ev.joinIsHost);
        if (ev.joinHasHeading)
            s_pendingHeading = ev.joinHeading;
        // E1: новая сетевая сессия — новая эпоха акторов, старых прокси нет.
        actor_sync::Reset();
    }
    if (ev.hostChanged)
    {
        const bool nowHost = ev.hostChangedIsHost;
        s_amHost.store(nowHost);
        std::snprintf(s_joinedName, sizeof(s_joinedName),
                      nowHost ? "You are now HOST" : "You are now GUEST");

        // E7.1-hardening (task-131): полный клиентский сброс при HostChanged.
        // world_sync: все world/vehicle-прокси и snapshots старого хоста.
        world_sync::Reset();
        // Visitor cleanup: очередь удаления старого мира/сцены не тащим.
        s_visitorCleanupCount = 0;
        s_visitorCleanupRanOnce = false;
        s_visitorScenePaused = false;

        // Mission barrier: не тащим offer/ready/window/start старого хоста.
        s_hostBarrier = HostMissionBarrier{};
        s_guestBarrier = GuestMissionBarrier{};
        {
            std::lock_guard<std::mutex> lk(s_barrierMtx);
            s_barrierEvents.clear();
        }
        s_lastHostBarrierOfferTick = 0;
        s_lastGuestBarrierPollTick = 0;

        // E6 presence: новая эпоха/флаги; удалённый статус сбрасываем.
        s_presenceEpoch = (s_presenceEpoch + 1u) | 1u;
        s_presenceSentValid = false;
        s_localPresenceFlags = 0;
        s_lastSentPresenceFlags = 0;
        s_lastPresenceDetectTick = 0;
        s_lastPresenceSendTick = 0;
        s_presenceWindowTick = 0;
        s_presenceWindowCount = 0;
        s_lastUiPresenceLogTick = 0;
        s_remotePresenceValid = false;
        s_remotePresenceFlags = 0;
        s_lastRemotePresenceTick = 0;

        // P2 Action/aim/shoot: очередь и состояния удалённого игрока.
        {
            std::lock_guard<std::mutex> lk(s_actionMtx);
            s_actionIn = net::ActionStatePayload{};
            s_haveActionIn = false;
        }
        s_lastSentAction = net::ActionStatePayload{};
        s_lastSentActionValid = false;
        s_lastActionSendTick = 0;
        s_appliedActionMode = 0;
        s_appliedScenario = 0;
        s_appliedTargetNetId = 0;
        s_lastLoggedTargetNetId = 0;
        s_lastCoverTaskTick = 0;
        s_remoteActionRaw = 0;
        s_remoteAim = Vec3{0.f, 0.f, 0.f};
        s_remoteAimValid = false;
        {
            std::lock_guard<std::mutex> lk(s_shotMtx);
            s_shotQCount = 0;
            s_shotEventsPending.store(false);
        }
        s_shotReplayPhase = ShotReplayPhase::Idle;
        s_shotReplayPhaseTick = 0;
        s_shotReplayRetried = false;
        s_shotReplayPoint = Vec3{0.f, 0.f, 0.f};
        s_lastShotEventTaskTick = 0;
        s_lastShotTick = 0;

        // Remote player vehicle (старый хост/гость мог быть в транспорте).
        {
            std::lock_guard<std::mutex> lk(s_pvMtx);
            s_pvIn = net::PlayerVehicleState{};
            s_havePvIn = false;
        }
        s_lastSentPvValid = false;
        s_remoteInVehicle = false;
        DeleteRemotePlayerVehicle();

        // Actor_sync::Reset чистит roster (corpse-удержания) и прокси-состояние
        // (motion-кольца/интерполяцию) — затем forced resend/resync по роли.
        actor_sync::Reset();
        if (nowHost)
            actor_sync::ForceFullSpawnBatch();   // новый хост: полный batch сразу
        else
            actor_sync::RequestImmediateResync(); // новый гость: сразу resync

        sh_log::Write("coop: role change -> %s reset complete\n",
                      nowHost ? "HOST" : "GUEST");
        s_subtitleTicksLeft = 10 * 60;
    }
    // E6 (v18): UI-статус удалённого игрока (для суффикса имени).
    if (ev.presenceValid)
    {
        s_remotePresenceValid = true;
        s_remotePresenceFlags = ev.presenceFlags;
        s_lastRemotePresenceTick = s_tickCount;
    }

    // Порядок важен: сначала connectionLost, потом leave, потом playerJoined
    // (события нового сеанса должны перебить старые).
    if (ev.connectionLost)
    {
        s_pendingConnectionLost = true;
        s_joinOkSeen = false; // E7.1: до reconnect гость не подавляет
        // E6: presence сбрасываем — после reconnect статус придёт заново.
        s_remotePresenceValid = false;
        s_remotePresenceFlags = 0;
        s_presenceSentValid = false;
        // После reconnect наряд/снаряжение надо переслать заново.
        s_sentOutfitValid = false;
        s_sentHorseOutfitValid = false;
        // E1: связь потеряна — удалить прокси/roster акторов.
        actor_sync::Reset();
        world_sync::Reset(); // E3d-b: снять mission-train + restore random trains
    }
    if (ev.leave)
    {
        std::snprintf(s_leaveName, sizeof(s_leaveName), "%s", ev.leaveName);
        s_pendingLeave = true;
        s_joinOkSeen = false; // E7.1: вышел из сессии — visitor mode off
        // E6: статус ушедшего игрока больше не показываем.
        s_remotePresenceValid = false;
        s_remotePresenceFlags = 0;
        s_presenceSentValid = false;
        // E1: игрок вышел — акторы устарели.
        actor_sync::Reset();
        world_sync::Reset(); // E3d-b: снять mission-train + restore random trains
    }
    if (ev.playerJoined)
    {
        sh_log::Write("coop: PLAYER_JOINED %s (model=0x%llX, pos=%d)\n",
                      ev.playerJoinedName, ev.playerJoinedModel,
                      ev.playerJoinedHasPos ? 1 : 0);
        std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s joined",
                      ev.playerJoinedName);
        s_playerJoinedNotified = true;
        s_presenceSentValid = false; // E6: новому игроку — свой статус заново
        s_remotePresenceValid = false;
        s_remotePresenceFlags = 0;
        s_pendingJoinWaitTicks = 0; // новый игрок — заново ждём его первый кадр
        s_subtitleTicksLeft = 8 * 60;
        std::snprintf(s_pedName, sizeof(s_pedName), "%s", ev.playerJoinedName);
        std::snprintf(s_pendingName, sizeof(s_pendingName), "%s", ev.playerJoinedName);
        // Модель из PLAYER_JOINED — это НЕ модель персонажа, а fallback из
        // INI/сервера. Валидируем и храним отдельно в s_joinedModel; реальную
        // модель (кадр reserved[2]) никогда не перезаписываем fallback'ом.
        s_joinedModel = 0;
        if (ev.playerJoinedModel && !IsModelHashValid(ev.playerJoinedModel))
        {
            sh_log::Write("coop: ignoring invalid join model 0x%llX\n",
                          ev.playerJoinedModel);
        }
        else
        {
            s_joinedModel = ev.playerJoinedModel;
        }
        if (!s_realModelSeen)
            s_remoteModel = s_joinedModel;
        else if (s_joinedModel && s_joinedModel != s_lastRealModel)
            sh_log::Write("coop: join fallback model 0x%llX ignored "
                          "(real 0x%llX known)\n",
                          s_joinedModel, s_lastRealModel);
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
        // E1: новый пир — пересоздать epoch/roster, чтобы сразу ушёл
        // ActorSpawnBatch (не ждать heartbeat).
        actor_sync::Reset();
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
    // до ~4 секунд: если пришёл — спавним сразу с моделью персонажа.
    // Если реальная модель уже известна (reconnect/повторный join) — не ждём:
    // сразу спавним с ней, иначе снова получим fallback+recreate.
    if (!s_ghost && s_playerJoinedNotified && !s_realModelSeen)
    {
        // Если кадр с моделью персонажа ещё не получен — ждём (но не вечно).
        if (!s_haveFrame || !s_lastFrame.reserved[2])
        {
            unsigned int waitTicks = s_pendingJoinWaitTicks++;
            // Ждём до ~4 с: если кадр с реальной моделью приходит позже 2 с,
            // спавн fallback-моделью давал одно видимое пересоздание.
            if (waitTicks < 4 * 60) // до ~4 секунд
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
            s_ghostNeedsBaseScrub = true;
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

    // Доиграть анимации оружия (доставание/убирание/перезарядка) — после
    // применения кадра, чтобы переходы этого тика успели начаться.
    UpdateGhostWeaponAnim();

    // ===== Реплей выстрелов у призрака: state machine с подтверждением =====
    // Пока Issued (выстрел не подтверждён) — никаких aim/move/heading/shoot:
    // новая TASK_* заменяет предыдущую и обрывает анимацию (из пачки
    // завершался только первый выстрел). IS_PED_SHOOTING у призрака —
    // единственный надёжный признак (GET_AMMO_IN_CLIP ненадёжен: infinite ammo).
    ULONGLONG nowMs = GetTickCount64();
    // 1) Слишком старые события (300-500 мс) не показываем — иначе «залпы».
    int droppedNow = 0;
    int shotPending = 0;
    unsigned long long oldestShotAgeMs = 0;
    {
        std::lock_guard<std::mutex> lk(s_shotMtx);
        while (s_shotQCount > 0 &&
               nowMs - s_shotQueueMs[0] > kShotReplayMaxAgeMs)
        {
            for (int i = 1; i < s_shotQCount; ++i)
            {
                s_shotQueue[i - 1] = s_shotQueue[i];
                s_shotQueueMs[i - 1] = s_shotQueueMs[i];
            }
            --s_shotQCount;
            ++droppedNow;
        }
        if (droppedNow > 0)
            s_replayDropped += static_cast<unsigned int>(droppedNow);
        if (s_shotQCount == 0)
            s_shotEventsPending.store(false);
        shotPending = s_shotQCount;
        if (s_shotQCount > 0 && nowMs >= s_shotQueueMs[0])
            oldestShotAgeMs = nowMs - s_shotQueueMs[0];
    }
    if (shotPending > 0)
        s_shotReplayLastEventTick = s_tickCount;

    // 2) Фактическое состояние призрака.
    const bool ghostExists = (s_ghost && DoesEntityExist(s_ghost));
    bool ghostShooting = false;
    bool ghostWeaponReady = false;
    if (ghostExists)
    {
        using namespace natives_api;
        Init(natives::IS_PED_SHOOTING); Push(s_ghost);
        ghostShooting = Call() != 0;
        Init(natives::IS_PED_WEAPON_READY_TO_SHOOT); Push(s_ghost);
        ghostWeaponReady = Call() != 0;
    }
    // Есть событие, но оружие «не готово» (после CLEAR/burst/смены оружия):
    // держим оружие в руках (SET_CURRENT — не task, конфликта с анимацией нет).
    // Иначе событие успевает «протухнуть» раньше ready-timeout.
    if (shotPending > 0 && ghostExists && !s_ghostDead && !ghostWeaponReady)
        EnsureGhostWeaponInHands();

    // 3) Переходы состояний + подтверждение выстрела / retry.
    switch (s_shotReplayPhase)
    {
    case ShotReplayPhase::Issued:
        if (ghostShooting)
        {
            s_shotReplayPhase = ShotReplayPhase::Confirmed;
            s_shotReplayPhaseTick = s_tickCount;
        }
        else if ((s_tickCount - s_shotReplayPhaseTick) >=
                 kShotReplayConfirmTimeoutTicks)
        {
            if (!s_shotReplayRetried)
            {
                // Одна повторная попытка с длинной анимацией.
                s_shotReplayRetried = true;
                s_shotReplayPhaseTick = s_tickCount;
                ShootGhostAt(s_shotReplayPoint, kShotReplayRetryDurationMs);
                sh_log::Write("coop: shot replay retry (no confirm) duration=%d\n",
                              kShotReplayRetryDurationMs);
            }
            else
            {
                // Выстрел так и не подтвердился — событие потеряно.
                ++s_replayDropped;
                s_shotReplayPhase = ShotReplayPhase::Idle;
                s_shotReplayPhaseTick = s_tickCount;
            }
        }
        break;
    case ShotReplayPhase::Confirmed:
        // Возвращаем управление, когда оружие доиграло (не стреляет и готово).
        if (!ghostShooting && ghostWeaponReady)
        {
            s_shotReplayPhase = ShotReplayPhase::Idle;
            s_shotReplayPhaseTick = s_tickCount;
        }
        else if ((s_tickCount - s_shotReplayPhaseTick) > kShotReplayConfirmMaxTicks)
        {
            s_shotReplayPhase = ShotReplayPhase::Idle; // safety: не зависаем
            s_shotReplayPhaseTick = s_tickCount;
        }
        break;
    case ShotReplayPhase::Idle:
    default:
        break;
    }

    // 4) AUTO-FIRE (burst): очередь >= threshold → ОДИН длинный TASK_SHOOT
    // (FullAuto) + SET_PED_SHOOT_RATE; прицел обновляем редко (>2 м и ~500 мс).
    bool burstEnded = false;
    // Началась посадка/спешивание или перезарядка — гасим burst, не
    // переиздавая задачи (TASK_SHOOT оборвёт анимацию).
    if (s_shotReplayBurst &&
        (GhostMountInTransition() || s_reloadInProgress ||
         s_weaponDrawPending || s_weaponHolsterPending ||
         s_weaponChangingNow))
    {
        s_shotReplayBurst = false;
        burstEnded = true;
    }
    if (s_shotReplayBurst)
    {
        if (ghostExists)
        {
            const Vec3 aim = GhostAimPointAhead();
            const float bdx = aim.x - s_shotReplayBurstPoint.x;
            const float bdy = aim.y - s_shotReplayBurstPoint.y;
            const float bdz = aim.z - s_shotReplayBurstPoint.z;
            const bool aimMoved =
                (bdx * bdx + bdy * bdy + bdz * bdz) >
                (kShotReplayBurstAimMoveDist * kShotReplayBurstAimMoveDist);
            // Держим длинную FullAuto-задачу живой: переиздаём раз в ~500 мс
            // (иначе duration 1.5 с истекает, а события ещё идут). Прицел
            // уточняем только при сдвиге >2 м.
            if ((s_tickCount - s_shotReplayBurstAimTick) >=
                kShotReplayBurstAimIntervalTicks)
            {
                s_shotReplayBurstAimTick = s_tickCount;
                if (aimMoved)
                    s_shotReplayBurstPoint = aim;
                const int burstDur = IsGhostMounted()
                    ? kShotReplayBurstDurationMountedMs
                    : kShotReplayBurstDurationMs;
                ShootGhostAt(s_shotReplayBurstPoint, burstDur,
                             kShotFiringPatternFullAuto);
                s_shotReplayBurstTick = s_tickCount;
            }
            // Продлеваем burst на каждом приходе события: при непрерывном
            // огне очередь не пустеет, и safety не должен рвать анимацию.
            if (shotPending > 0)
                s_shotReplayBurstTick = s_tickCount;
            if (shotPending == 0 &&
                (s_tickCount - s_shotReplayLastEventTick) >
                    kShotReplayBurstIdleTicks)
            {
                s_shotReplayBurst = false;
                burstEnded = true;
            }
            else if ((s_tickCount - s_shotReplayBurstTick) >
                     kShotReplayBurstMaxTicks)
            {
                s_shotReplayBurst = false; // safety (1200 тиков = 20 с)
                burstEnded = true;
            }
        }
        else
        {
            s_shotReplayBurst = false;
            burstEnded = true;
        }
        if (s_shotReplayBurst)
            shotPending = 0; // coalesce: per-shot в burst-режиме не гоняем
    }
    // Конец burst: сбрасываем shoot rate (иначе ghost стреляет чаще нормы),
    // снимаем длинную FullAuto-задачу. На лошади CLEAR_PED_TASKS ломает
    // посадку — вместо него пересаживаем всадника (обрывает хвост).
    if (burstEnded && ghostExists)
    {
        // 100 = default (SDK: 0 = самый медленный); иначе per-shot после
        // burst может не успевать.
        SetGhostShootRate(kGhostShootRateDefault);
        // Перезарядка: не сбрасываем латч/задачи (иначе reload оборвётся).
        if (!s_reloadInProgress)
            InvalidateGhostWeaponLatch();
        s_shotReplayReadyFailSince = 0;
        const bool mounted = IsGhostMounted();
        // Идёт посадка/спешивание — НЕ трогаем задачи (CLEAR/Promote оборвут
        // TASK_MOUNT_ANIMAL/TASK_DISMOUNT_ANIMAL и сломают анимацию).
        const bool mountTransition = GhostMountInTransition();
        // Анимацию перезарядки CLEAR_PED_TASKS тоже оборвал бы.
        const bool reloadTransition = s_reloadInProgress;
        // CLEAR не делаем в транспорте/сценарии/сидении — там свои задачи.
        const bool inScenario =
            s_remoteActionRaw == static_cast<uint8_t>(net::Action::InScenario) ||
            s_remoteActionRaw == static_cast<uint8_t>(net::Action::Sitting) ||
            s_remoteActionRaw == static_cast<uint8_t>(net::Action::Drunk);
        if (mountTransition || reloadTransition)
        {
            // ничего не снимаем/не пересаживаем — дадим анимации доиграть
        }
        else if (!mounted && !s_remoteInVehicle && !inScenario)
        {
            using namespace natives_api;
            Init(natives::CLEAR_PED_TASKS);
            Push(s_ghost);
            Push(static_cast<unsigned long long>(1));
            Push(static_cast<unsigned long long>(1));
            Call();
            // Задача движения снята — заставим ApplyMoveToGhost выдать её заново.
            s_lastMoveState = 0xFF;
            s_moveTargetValid = false;
            s_combatActive = false;
        }
        else if (mounted)
        {
            // Пересаживаем: SET_PED_ONTO_MOUNT заменяет FullAuto-задачу и не
            // даёт ей достреливать 0.7-1.5 с на лошади.
            PromoteMountToGhost();
            s_mountTaskValid = false;
        }
    }

    // 5) Per-shot: только в Idle и пока очередь меньше burst-порога.
    // Во время посадки/спешивания задачи не выдаём (события пусть стареют —
    // иначе TASK_SHOOT оборвёт TASK_MOUNT_ANIMAL/TASK_DISMOUNT_ANIMAL).
    if (!s_shotReplayBurst && s_shotReplayPhase == ShotReplayPhase::Idle &&
        ghostExists && !s_ghostDead && s_shootHoldTicks == 0 &&
        !GhostMountInTransition() && !s_reloadInProgress &&
        !s_weaponDrawPending && !s_weaponHolsterPending &&
        !s_weaponChangingNow)
    {
        // «Готов к выстрелу» с таймаутом: лук/не-firearm не должен
        // заблокировать реплей навсегда. Если очередь пуста — таймер
        // неудачного ready сбрасываем (иначе он «протухнет» между сериями).
        if (shotPending == 0)
            s_shotReplayReadyFailSince = 0;
        else if (ghostWeaponReady)
            s_shotReplayReadyFailSince = 0;
        else if (s_shotReplayReadyFailSince == 0)
            s_shotReplayReadyFailSince = s_tickCount;
        const bool readyTimeout =
            !ghostWeaponReady && s_shotReplayReadyFailSince != 0 &&
            (s_tickCount - s_shotReplayReadyFailSince) >=
                kShotReplayReadyTimeoutTicks;
        // Самое старое событие «стареет» — форсируем выдачу без ожидания ready,
        // иначе оно будет выкинуто по возрасту (первый выстрел после
        // респавна/смены оружия/CLEAR терялся). forceByAge отменяет ТОЛЬКО
        // ready — минимальный интервал между задачами всё равно соблюдаем.
        const bool forceByAge = (oldestShotAgeMs > kShotReplayForceAgeMs);
        const bool readyOk = ghostWeaponReady || readyTimeout || forceByAge;
        const unsigned int minInterval = forceByAge
            ? kShotReplayForceMinIntervalTicks
            : kShotReplayIntervalTicks;
        const bool intervalDone =
            (s_tickCount - s_lastShotEventTaskTick) >= minInterval;

        if (shotPending >= kShotReplayBurstThreshold && intervalDone && readyOk)
        {
            // Авто-огонь: один длинный FullAuto task вместо серии коротких.
            const Vec3 aim = GhostAimPointAhead();
            {
                std::lock_guard<std::mutex> lk(s_shotMtx);
                s_shotQCount = 0;
            }
            s_shotEventsPending.store(false);
            const int burstDur = IsGhostMounted()
                ? kShotReplayBurstDurationMountedMs
                : kShotReplayBurstDurationMs;
            ShootGhostAt(aim, burstDur, kShotFiringPatternFullAuto);
            SetGhostShootRate(kShotReplayBurstShootRate);
            s_shotReplayBurst = true;
            s_shotReplayBurstTick = s_tickCount;
            s_shotReplayBurstAimTick = s_tickCount;
            s_shotReplayBurstPoint = aim;
            s_shotReplayLastEventTick = s_tickCount;
            s_lastShotEventTaskTick = s_tickCount;
            s_lastShootTaskTick = s_tickCount;
            s_combatActive = true;
            s_lastAction = static_cast<uint8_t>(net::Action::Shooting);
            sh_log::Write("coop: shot replay burst start (fullauto, rate=%d)\n",
                          kShotReplayBurstShootRate);
        }
        else if (shotPending > 0 && intervalDone && readyOk)
        {
            Vec3 t{0.f, 0.f, 0.f};
            bool gotShot = false;
            {
                std::lock_guard<std::mutex> lk(s_shotMtx);
                if (s_shotQCount > 0)
                {
                    t = s_shotQueue[0];
                    for (int i = 1; i < s_shotQCount; ++i)
                    {
                        s_shotQueue[i - 1] = s_shotQueue[i];
                        s_shotQueueMs[i - 1] = s_shotQueueMs[i];
                    }
                    --s_shotQCount;
                    if (s_shotQCount == 0)
                        s_shotEventsPending.store(false);
                    gotShot = true;
                }
            }
            if (gotShot)
            {
                if (t.x == 0.f && t.y == 0.f && t.z == 0.f)
                    t = GhostAimPointAhead(); // нет данных — вперёд
                ShootGhostAt(t, kShotReplayShotDurationMs,
                             kShotFiringPatternSingleShot);
                s_shotReplayPhase = ShotReplayPhase::Issued;
                s_shotReplayPhaseTick = s_tickCount;
                s_shotReplayRetried = false;
                s_shotReplayPoint = t;
                s_lastShotEventTaskTick = s_tickCount;
                s_lastShootTaskTick = s_tickCount;
                s_combatActive = true;
                s_lastAction = static_cast<uint8_t>(net::Action::Shooting);
                sh_log::Write("coop: shot event -> (%.1f %.1f %.1f)\n",
                              t.x, t.y, t.z);
            }
        }
    }

    // 6) Лог отброшенных: переполнение из сетевого потока + поздние/
    // неподтверждённые события реплея.
    if ((s_replayDropped > 0 || s_shotDropped.load() > 0) &&
        (s_tickCount - s_lastReplayDropLogTick) >= 120)
    {
        s_lastReplayDropLogTick = s_tickCount;
        const int totalDropped =
            static_cast<int>(s_replayDropped) + s_shotDropped.load();
        sh_log::Write("coop: shot replay dropped=%d queued=%d\n",
                      totalDropped, shotPending);
        s_replayDropped = 0;
        s_shotDropped.store(0);
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
            // E6 (v18): суффикс UI-статуса удалённого игрока. s_pedName не
            // мутируем (его используют логи/persist) — собираем буфер.
            const char* displayName = s_pedName;
            if (coopcfg::Get().npcActor && coopcfg::Get().npcPresence &&
                s_remotePresenceValid)
            {
                const char* suffix = PresenceSuffix(s_remotePresenceFlags);
                if (suffix[0])
                {
                    std::snprintf(s_presenceNameBuf,
                                  sizeof(s_presenceNameBuf), "%s - %s",
                                  s_pedName, suffix);
                    displayName = s_presenceNameBuf;
                }
            }
            DrawNameAt(aboveHead, displayName);
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

    // E1: репликация NPC-актёров (host sender + guest proxies + lifecycle).
    // При npc_actor=0 actor_sync::Tick сразу выходит — ноль нативов/логов.
    // Вызов — в конце Tick, после базовой обработки, до persist.
    {
        actor_sync::TickContext actx;
        actx.localPed = playerPed;
        actx.tickCount = s_tickCount;
        actx.amHost = s_amHost.load();
        actx.enabled = coopcfg::Get().npcActor;
        actx.partnerValid = s_lastKnownPosValid;
        actx.partnerX = s_lastKnownPos.x;
        actx.partnerY = s_lastKnownPos.y;
        actx.partnerZ = s_lastKnownPos.z;
        actx.partnerGhost = s_ghost;
        actx.partnerMount = s_mount;
        {
            const unsigned long long start = QpcNow();
            actor_sync::Tick(actx);
            perfActorUs = QpcDeltaUs(start);
        }
    }

    // Persist the ped's position regularly so a returning player spawns on
    // their last known coords. Write at a low frequency to avoid hammering
    // the file (every ~5s at 60 ticks/s).
    TickPersistGhostPosition();

    // Отправка собственного GhostFrame на сервер (каждые ~4 тика, чтобы
    // сервер видел координаты игрока в таблице состояния и ретранслировал
    // их другим игрокам). В кадре передаём move/state/flags/health игрока.
    SendLocalPlayerFrame(playerPed);

    SyncLocalOutfit(playerPed);

    // Синхронизация мира (время + погода): хост шлёт своё состояние каждые
    // ~5 секунд, остальные применяют его у себя (SET_CLOCK_TIME / погода).
    TickWorldStateSync();

    // Small "Player X joined" subtitle on screen while a join happened
    // (drawn each frame for a short while proves the text path works).
    // Гейт по тексту/таймеру, а не по s_playerJoinedNotified: этот флаг теперь
    // означает только «удалённый игрок в сессии» (гейт спавна), а субтитры
    // показываем и для death/respawn/host-changed/connection-lost.
    TickJoinSubtitle();

    // Редкий heartbeat-лог живого педа (раньше раз в 2 с засорял лог).
    if (s_tickCount % (30 * 60) == 1 && s_ghost && DoesEntityExist(s_ghost))
        sh_log::Write("coop: ghost ped alive, handle=%u\n", s_ghost);

    // Задержка до сервера: окно мин/макс обновляется раз в 2 с, но строка
    // РИСУЕТСЯ КАЖДЫЙ КАДР — _BG_DISPLAY_TEXT живёт один кадр, иначе надпись
    // мелькает миллисекунду и пропадает.
    TickRttHud();

    TickPerfWindow(perfFrameStart, perfWorldUs, perfActorUs);
}


bool IsVisitorVendorModel(uint32_t model)
{
    return IsVendorModel(model);
}
} // namespace coop
