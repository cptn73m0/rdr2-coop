#include "coop.h"
#include "config.h"
#include "ghost_frame.h"
#include "log.h"
#include "natives.h"
#include "net/client.h"
#include "persist.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

void RequestModel(unsigned long long model)
{
    using namespace natives_api;
    Init(natives::REQUEST_MODEL);
    Push(model);
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
    Push(true);
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
    Push(0.5f); Push(0.5f); // заметнее, чем 0.4 (надпись легко пропустить)
    Call();
    Init(natives::UI_BG_SET_TEXT_COLOR);
    Push(255); Push(255); Push(255); Push(255);
    Call();
    Init(natives::UI_BG_DISPLAY_TEXT);
    Push(strHandle);
    Push(sx);
    Push(sy);
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
    Push(0.35f); Push(0.35f);
    Call();
    Init(natives::UI_BG_SET_TEXT_COLOR);
    Push(255); Push(230); Push(80); Push(255); // yellow-ish
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

// Состояние для спавна от сервера (заполняется колбэком, который бежит в
// СЕТЕВОМ потоке — здесь НЕЛЬЗЯ вызывать нативы! Все нативы — в Tick()).
bool   s_pendingJoin = false;
float  s_pendingHeading = 0;
char   s_pendingName[64] = "";
unsigned long long s_remoteModel = 0; // модель удалённого игрока (из PLAYER_JOINED)
// Координаты спавна педа удалённого игрока (из PLAYER_JOINED, если сервер
// прислал его последние/сохранённые координаты; иначе спавним у хоста).
bool   s_pendingHasPos = false;
float  s_pendingX = 0, s_pendingY = 0, s_pendingZ = 0;

// Последний кадр от удалённого игрока (для применения в Tick()).
net::FramePayload s_lastFrame{};
bool s_haveFrame = false;
// Флаг «игрок вышел»: обработка (удаление педа) произойдёт в Tick().
bool s_pendingLeave = false;
char s_leaveName[64] = ""; // имя ушедшего игрока (для надписи «left»)
// Последнее применённое к педу состояние движения (сброс при выходе).
uint8_t s_lastMoveState = 0xFF;
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

// Применение позиции из сетевого кадра к педу призрака.
// Вызывается ИЗ Tick() (игровой поток).
// Движение ИНТЕРПОЛИРУЕТСЯ: за каждый тик пед смещается на часть пути к цели
// (без телепортов). Полный телепорт — только если скачок большой (спавн
// далеко от текущей точки, напр. из players.txt).
void ApplyRemoteFrame(const net::FramePayload& f)
{
    if (!s_ghost || !DoesEntityExist(s_ghost)) return;
    // Мёртвого педа не двигаем: труп лежит там, где умер (иначе он будет
    // «прыгать» по кадрам удалённого игрока и парить над землёй).
    if (s_ghostDead) return;

    constexpr float LERP_FACTOR = 0.25f; // за тик проходит ~25% оставшегося пути
    constexpr float TELEPORT_DIST = 10.0f; // больше этой дистанции — телепорт

    Vec3 cur = GetEntityCoords(s_ghost);
    Vec3 p{f.x, f.y, f.z};

    const uint8_t move = static_cast<uint8_t>(f.move);
    if (move == static_cast<uint8_t>(coop::MoveState::Walk) ||
        move == static_cast<uint8_t>(coop::MoveState::Run))
    {
        // Движение задачей: не телепортируем и не лерпим — просто
        // перенаправляем задачу идти к текущей точке кадра (топает ножками).
        ApplyMoveToGhost(s_ghost, move, p);
    }
    else
    {
        // Idle: плавно подходим к XY цели кадра с фиксированным шагом ЗА ТИК
        // (а не «25% за кадр» — кадры приходят редко, и каждый давал рывок).
        // Z не трогаем (пеd стоит на земле; на Z кадра — «пояс» игрока, он
        // выше земли). Когда пед уже близко к цели (<= CLOSE), НЕ двигаем
        // вовсе — иначе каждый входящий кадр дёргает его (прыжки на месте).
        const float CLOSE = 0.5f;      // дистанция XY, ближе которой стоим
        const float STEP  = 0.15f;     // метров за тик (при 60 FPS ~9 м/с)
        float dx = p.x - cur.x, dy = p.y - cur.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        bool moved = false;
        if (dist > TELEPORT_DIST)
        {
            // Резкий перенос: XY из кадра, Z сохраняем (землю).
            SetEntityCoords(s_ghost, Vec3{p.x, p.y, cur.z});
            moved = true;
        }
        else if (dist > CLOSE)
        {
            float s = std::min(STEP, dist);
            Vec3 n;
            n.x = cur.x + dx / dist * s;
            n.y = cur.y + dy / dist * s;
            n.z = cur.z;
            SetEntityCoords(s_ghost, n);
            moved = true;
        }
        if (moved)
        {
            // Ставим педа на ЗЕМЛЮ: берём высоту земли в точке XY (Z кадра —
            // «пояс» игрока, выше земли) и опускаем педа на неё. Заморозку
            // на время снимаем, иначе PRONE/прижим не сработает.
            float groundZ = 0.f;
            bool hasGround = false;
            {
                using namespace natives_api;
                Init(natives::GET_GROUND_Z_FOR_3D_COORD);
                Push(p.x); Push(p.y); Push(cur.z);
                Push(reinterpret_cast<unsigned long long>(&groundZ));
                unsigned long long ok = Call();
                hasGround = ok != 0;
            }
            if (hasGround)
            {
                SetGhostFrozen(false);
                SetEntityCoords(s_ghost, Vec3{p.x, p.y, groundZ + 0.2f});
                using namespace natives_api;
                Init(natives::PLACE_OBJECT_ON_GROUND_PROPERLY);
                Push(s_ghost);
                Call();
                SetGhostFrozen(true);
            }
        }
        ApplyMoveToGhost(s_ghost, move, p);
    }

    // Поворот интерполируем мягко: кратчайшая дуга.
    float h = f.heading - GetEntityHeading(s_ghost);
    while (h > 180.f) h -= 360.f;
    while (h < -180.f) h += 360.f;
    SetEntityHeading(s_ghost, GetEntityHeading(s_ghost) + h * LERP_FACTOR);
}

// Удалить педа и метку, сбросить флаги спавна (игровой поток).
void CleanupGhost()
{
    if (s_ghost && DoesEntityExist(s_ghost))
    {
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
    }
    s_ghost = 0;
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
}

// Определить move-состояние локального игрока для кадра (передаётся
// удалённой стороне для выбора анимации педа). Приоритет: верховая езда >
// бег > ходьба > стояние.
uint8_t DetectLocalMoveState(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_ON_MOUNT); Push(ped);
    if (Call()) return static_cast<uint8_t>(coop::MoveState::RideHorse);
    Init(natives::IS_PED_RUNNING); Push(ped);
    if (Call()) return static_cast<uint8_t>(coop::MoveState::Run);
    Init(natives::IS_PED_WALKING); Push(ped);
    if (Call()) return static_cast<uint8_t>(coop::MoveState::Walk);
    return static_cast<uint8_t>(coop::MoveState::Idle);
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
    if (move == walkState || move == runState)
    {
        // Движение задачей (топает ножками). Физику педа НЕ замораживаем,
        // иначе TASK_GO_* не сможет его вести.
        if (s_lastMoveState != move)
        {
            SetGhostFrozen(false);
            s_lastMoveState = move;
            s_moveTargetValid = false; // принудим перепостановку задачи
        }
        // Считаем, нужно ли ПЕРЕдавать задачу: только если сменилось
        // состояние движения ИЛИ цель ушла дальше ~0.3 м от предыдущей.
        bool retarget = false;
        if (move != s_lastMoveState)
            retarget = true;
        else if (!s_moveTargetValid ||
                 std::fabs(target.x - s_moveTarget.x) > 0.3f ||
                 std::fabs(target.y - s_moveTarget.y) > 0.3f)
            retarget = true;

        if (retarget)
        {
            const float speed = (move == runState) ? 6.f : 2.5f;
            Init(natives::TASK_GO_STRAIGHT_TO_COORD);
            Push(ped);
            Push(target.x); Push(target.y); Push(target.z);
            Push(speed);                    // moveBlendSpeed (скорость анимации)
            Push(static_cast<unsigned long long>(0xFFFFFFFF)); // p5 timeBeforeTeleport = -1
            Push(0.f);                      // p6 finalHeading
            Push(0.5f);                     // p7 targetRadius
            Push(0);                        // p8
            Call();
            s_moveTarget = target;
            s_moveTargetValid = true;
        }
        s_lastMoveState = move;
    }
    else // Idle / RideHorse (верховую анимацию не делаем, просто стоим)
    {
        if (s_lastMoveState != move)
        {
            // Стоящий пед замораживаем: движок не толкает его коллизиями
            // игрока/физикой — он стоит как статуя, пока кадры не переместят.
            SetGhostFrozen(true);
            s_lastMoveState = move;
            Init(natives::TASK_STAND_STILL);
            Push(ped); Push(static_cast<unsigned long long>(0xFFFFFFFF));
            Call();
        }
    }
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
        sh_log::Write("net: JOIN_OK (hasSaved=%d) from server\n", jr.hasSaved ? 1 : 0);
        s_pendingJoin = true;
        if (jr.hasSaved)
            s_pendingHeading = jr.heading;
    });

    g_netClient.SetPlayerJoin([](const netio::PlayerJoinInfo& ji) {
        sh_log::Write("net: PLAYER_JOINED %s (model=0x%llX, pos=%d)\n",
                      ji.name.c_str(), ji.model, ji.hasPos ? 1 : 0);
        std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s joined", ji.name.c_str());
        s_playerJoinedNotified = true;
        s_subtitleTicksLeft = 8 * 60;
        std::snprintf(s_pedName, sizeof(s_pedName), "%s", ji.name.c_str());
        std::snprintf(s_pendingName, sizeof(s_pendingName), "%s", ji.name.c_str());
        s_remoteModel = ji.model;
        if (ji.hasPos)
        {
            s_pendingHasPos = true;
            s_pendingX = ji.x; s_pendingY = ji.y; s_pendingZ = ji.z;
            s_pendingHeading = ji.heading;
        }
        else
        {
            s_pendingHasPos = false;
        }
    });

    g_netClient.SetFrame([](const net::FramePayload& f) {
        // Только копируем кадр; нативы применит Tick().
        s_lastFrame = f;
        s_haveFrame = true;
    });

    g_netClient.SetPlayerLeave([](const std::string& name) {
        sh_log::Write("net: PLAYER_LEFT %s\n", name.c_str());
        std::snprintf(s_leaveName, sizeof(s_leaveName), "%s", name.c_str());
        s_pendingLeave = true; // физическое удаление сделает Tick()
    });

    // Запуск с параметрами из rdr2-coop.ini (адрес/порт/ник/модель).
    coopcfg::Config& cfg = coopcfg::Load();
    sh_log::Write("net: connecting to %s:%d as '%s' (model=0x%llX)\n",
                  cfg.serverHost.c_str(), cfg.serverPort,
                  cfg.playerName.c_str(), cfg.pedModel);
    g_netClient.Start(cfg.serverHost, cfg.serverPort, cfg.playerName, cfg.pedModel);
}

} // namespace

void Tick()
{
    ++s_tickCount;
    unsigned int playerPed = GetPlayerPedFor(0);
    if (!playerPed)
        return;

    // Start the network client on first tick (replaces the old offline
    // emulation as the source of join events).
    InitNetworkClient();

    // Модель для педа: чужая (из PLAYER_JOINED), иначе дефолтная.
    unsigned long long wantModel = s_remoteModel ? s_remoteModel : GHOST_PED_MODEL;
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

    // Обработка выхода игрока (флаг ставит сетевой колбэк, всё удаление —
    // здесь, на игровом потоке). Делаем ДО спавна, чтобы новый JOIN не
    // перебился LEAVE от старого клиента.
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

    // Спавн педа: только при PLAYER_JOINED (не при своём JOIN_OK).
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
        s_ghost = CreateGhostPed(wantModel, spawn, heading);
        sh_log::Write(s_ghost ? "coop: ghost ped created (on join), handle=%u at (%.1f %.1f %.1f)\n"
                              : "coop: CREATE_PED returned 0\n",
                      s_ghost, spawn.x, spawn.y, spawn.z);
        if (s_ghost)
        {
            SetEntityMission(s_ghost);
            RandomizePedComponents(s_ghost);
            SetEntityVisibleForce(s_ghost);
            PlaceOnGround(s_ghost);
            TaskStandStill(s_ghost);
            SetGhostFrozen(true); // дефолт для Idle; кадры с Walk/Run разморозят
            s_blip = CreateFriendlyBlipAt(spawn);
            sh_log::Write("coop: friendly blip -> %u\n", s_blip);
            std::snprintf(s_pedName, sizeof(s_pedName), "%s", name);
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
        ApplyRemoteFrame(s_lastFrame);
        s_haveFrame = false;
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
            aboveHead.z += 1.9f; // roughly above a standing human
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
    // короткое оповещение (однократно).
    if (s_ghost && DoesEntityExist(s_ghost) && IsPedDeadOrDying(s_ghost) && !s_ghostDead)
    {
        s_ghostDead = true;
        sh_log::Write("coop: [DEAD] %s\n", s_pedName[0] ? s_pedName : "Player");
        if (s_blip)
            s_blip = RemoveBlip(s_blip);
        // короткое оповещение, как при подключении
        std::snprintf(s_joinedName, sizeof(s_joinedName), "Player %s is dead",
                      s_pedName[0] ? s_pedName : "Player");
        s_playerJoinedNotified = true; // переиспользуем механизм таймера оповещения
        s_subtitleTicksLeft = 8 * 60;
        s_pedName[0] = '\0'; // имя над головой скрыто
    }

    // Смерть локального игрока: сообщить серверу (лог в консоли), однократно.
    if (g_netStarted && !s_localDiedNotified &&
        DoesEntityExist(playerPed) && IsPedDeadOrDying(playerPed))
    {
        s_localDiedNotified = true;
        g_netClient.SendDied();
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
    // их другим игрокам). В кадре передаём move-состояние игрока (анимация).
    if (g_netStarted && (s_tickCount % 4 == 1))
    {
        Vec3 pc = GetEntityCoords(playerPed);
        net::FramePayload fp{};
        fp.x = pc.x; fp.y = pc.y; fp.z = pc.z;
        fp.heading = GetEntityHeading(playerPed);
        fp.move = DetectLocalMoveState(playerPed);
        fp.seq = s_tickCount;
        g_netClient.SendFrame(fp);
    }

    // Small "Player X joined" subtitle on screen while a join happened
    // (drawn each frame for a short while proves the text path works).
    if (s_playerJoinedNotified && s_joinedName[0])
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
            s_joinedName[0] = '\0'; // hide permanently until next join
        }
    }

    if (s_tickCount % 120 == 1 && s_ghost && DoesEntityExist(s_ghost))
        sh_log::Write("coop: ghost ped alive, handle=%u\n", s_ghost);
}

} // namespace coop
