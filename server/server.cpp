// RDR2 CO-OP — сервер-релей.
// Кроссплатформенное консольное приложение (без GUI): слушает TCP-порт,
// принимает клиентов (ASI-плагины), обрабатывает JOIN/FRAME/LEAVE и
// пересылает FRAME между клиентами. Хранит last-position игроков.
//
// В окне консоли видны: логи подключений/отключений, IP и ники игроков,
// периодическая таблица состояния с координатами игроков.
//
// Сборка (Windows-first): см. server/CMakeLists.txt.

#include "common.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#endif

#include "net/frame.h"
#include "net/protocol.h"
#include "net/socket.h"
#include "mission_barrier_rules.h"

using netio::Result;

namespace {

// ---------------- Состояние сервера ----------------
// ub-mode (task-165): окно rate-limit «N сообщений в секунду на клиента».
// Поля защищены g_mutex; AllowRate() — одна точка логики окна.
struct RateWindow
{
    uint64_t windowMs = 0;
    int count = 0;
};

struct OnlinePlayer {
    std::string name;
    std::string ip;
    uint64_t sessionId = 0;
    unsigned long long pedModel = 0;
    netio::Socket* sock = nullptr;
    float x = 0, y = 0, z = 0, heading = 0;
    bool hasLoc = false;
    bool isHost = false;   // первый подключившийся (его время/погода = канонические)
    // ub-mode (task-165): окна rate-limit — по одному RateWindow на тип
    // сообщения вместо пары window/count (защищены g_mutex).
    RateWindow damageRate;    // E5 (v17): ActorDamage 0x1F
    RateWindow presenceRate;  // E6 (v18): PlayerPresence 0x20
    RateWindow occupantsRate; // E3d-c (v19): VehicleOccupants 0x21
    RateWindow trainV2Rate;   // E3d-b (v19): TrainSnapshotV2 0x22
    RateWindow interactionRate;       // v20 P1: VehicleInteraction 0x23 (guest)
    RateWindow interactionResultRate; // v20 P1: VehicleInteractionResult 0x24
    RateWindow vehicleStateRate;      // v20 P1: VehicleState 0x26
    // Host migration hardening: время последнего игрового (не Ping) сообщения
    // от этого клиента (мс). Если isHost и с этого момента прошло больше
    // g_hostGameTimeoutMs — хост считается зависшим и мигрирует.
    uint64_t lastGameMs = 0;
    // task-175: грейс хоста до первого game-thread сообщения. joinMs — момент
    // JOIN/промоушена; sawGameMsg — приходило ли уже такое сообщение.
    uint64_t joinMs = 0;
    bool sawGameMsg = false;
};

std::mutex g_mutex;                       // защищает g_online и g_lastPos
std::mutex g_saveMutex;                   // сериализует SaveAll (tmp+rename)
std::mutex g_logMutex;                    // защищает вывод в консоль
std::map<std::string, OnlinePlayer> g_online;   // игроки онлайн (по имени)
std::map<std::string, OnlinePlayer> g_lastPos;  // последние позиции (персистентность)
std::atomic<bool> g_running{true};
std::atomic<int> g_activeConnections{0};
std::atomic<int> g_preJoinConnections{0};
std::atomic<int> g_peakConnections{0};
std::atomic<uint64_t> g_totalConnections{0};
std::atomic<uint64_t> g_rejectedRate{0};
std::atomic<uint64_t> g_rejectedCap{0};
std::atomic<uint64_t> g_rejectedPreJoin{0};
std::atomic<uint64_t> g_warnCounter{0};

// P0 (task-155): фактический сетевой трафик — ТОЛЬКО метрики, wire v19 не
// меняется. bytes_in/out считаются на уровне кадров протокола, включая
// framing 5 B (len u32 + type u8), чтобы G3 был сопоставим с клиентом.
std::atomic<uint64_t> g_netBytesIn{0};
std::atomic<uint64_t> g_netBytesOut{0};
enum class TrafficBucket : int
{
    Frame = 0,
    Actor,
    World,
    Outfit,
    Presence,
    Damage,
    Trains,
    Occupants,
    Vehicle, // v20 P1: 0x23/0x24/0x26
    Other,
    Count
};
std::atomic<uint64_t> g_netTypeCounts[static_cast<int>(TrafficBucket::Count)];

TrafficBucket TrafficBucketFor(net::Msg m)
{
    switch (m)
    {
    case net::Msg::Frame: return TrafficBucket::Frame;
    case net::Msg::ActorSpawnBatch:
    case net::Msg::ActorFrames:
    case net::Msg::ActorDespawn:
    case net::Msg::ActorEvent:
    case net::Msg::ActorAppearance:
    case net::Msg::ActorResync: return TrafficBucket::Actor;
    case net::Msg::WorldState:
    case net::Msg::WorldSnapshot:
    case net::Msg::ActionState:
    case net::Msg::PlayerVehicle:
    case net::Msg::Shot:
    case net::Msg::MissionEvent:
    case net::Msg::MissionState:
    case net::Msg::MissionBarrier: return TrafficBucket::World;
    case net::Msg::Outfit:
    case net::Msg::HorseOutfit:
    case net::Msg::ActorOutfit: return TrafficBucket::Outfit;
    case net::Msg::PlayerPresence: return TrafficBucket::Presence;
    case net::Msg::ActorDamage: return TrafficBucket::Damage;
    case net::Msg::TrainSnapshot:
    case net::Msg::TrainSnapshotV2: return TrafficBucket::Trains;
    case net::Msg::VehicleOccupants: return TrafficBucket::Occupants;
    case net::Msg::VehicleInteraction:
    case net::Msg::VehicleInteractionResult:
    case net::Msg::VehicleState: return TrafficBucket::Vehicle;
    default: return TrafficBucket::Other;
    }
}

void CountInboundTraffic(net::Msg m, std::size_t payloadSize)
{
    g_netBytesIn.fetch_add(net::kHeaderSize + payloadSize,
                           std::memory_order_relaxed);
    g_netTypeCounts[static_cast<int>(TrafficBucketFor(m))].fetch_add(
        1, std::memory_order_relaxed);
}

// Единая точка отправки: без неё g_netBytesOut будет неполным.
void SendTracked(netio::Socket* sock, const uint8_t* data, int len)
{
    if (!sock || !data || len <= 0)
        return;
    sock->Send(data, len);
    g_netBytesOut.fetch_add(static_cast<uint64_t>(len),
                            std::memory_order_relaxed);
}

// ub-mode (task-165): единая логика окна rate-limit (вызывать под g_mutex).
bool AllowRate(RateWindow& w, int limit, uint64_t nowMs)
{
    if (w.windowMs == 0 || nowMs - w.windowMs >= 1000)
    {
        w.windowMs = nowMs;
        w.count = 0;
    }
    if (w.count >= limit) return false;
    ++w.count;
    return true;
}

// Защита от thread/socket flood: больше этого числа TCP-соединений сервер
// не держит. Легитимных участников максимум двое; остальное — probe/rogue.
constexpr int kMaxActiveConnections = 32;
// Отдельный лимит «ещё не JOIN-нутых» сокетов: иначе один клиент может
// занять все 32 слота и не дать подключиться реальным игрокам.
constexpr int kMaxPreJoinConnections = 8;
// Сколько ждём JOIN после установки TCP: без этого pre-JOIN сокет живёт до
// общего idle timeout и держит слот.
constexpr uint64_t kPreJoinTimeoutMs = 5000;
// Ping — только маленькое RTT-эхо; крупные payload не отражаем (анти-OOM).
constexpr std::size_t kMaxPingEchoBytes = 64;
// Per-IP anti-flood: не больше N новых TCP-соединений в секунду с одного IP.
constexpr int kMaxNewConnectionsPerIpPerSecond = 12;
// E5 (v17): не больше N запросов урона (0x1F) в секунду от одного клиента,
// чтобы спам не занимал канал и не нагружал хоста вхолостую.
constexpr int kMaxActorDamagePerSecond = 32;
// E6 (v18): UI-статус — не больше N сообщений (0x20) в секунду от клиента.
constexpr int kMaxPlayerPresencePerSecond = 10;
// E3d-c (v19): occupants — не больше 20 сообщений (0x21) в секунду с клиента.
constexpr int kMaxVehicleOccupantsPerSecond = 20;
// E3d-b (v19): поезд v2 — не больше 4 сообщений (0x22) в секунду с клиента.
constexpr int kMaxTrainSnapshotV2PerSecond = 4;
// v20 P1 (D3): rate-лимиты сервера для интерактивности транспорта.
constexpr int kMaxVehicleInteractionPerSecond = 8;
constexpr int kMaxVehicleInteractionResultPerSecond = 20;
constexpr int kMaxVehicleStatePerSecond = 10;
std::mutex g_rateMutex;
struct IpRate
{
    uint64_t windowMs = 0;
    int count = 0;
};
std::unordered_map<std::string, IpRate> g_ipRates;

bool AllowConnectionFrom(const std::string& ip, uint64_t nowMs)
{
    std::lock_guard<std::mutex> lk(g_rateMutex);
    IpRate& r = g_ipRates[ip];
    if (r.windowMs == 0 || nowMs - r.windowMs >= 1000)
    {
        r.windowMs = nowMs;
        r.count = 0;
    }
    if (r.count >= kMaxNewConnectionsPerIpPerSecond)
        return false;
    ++r.count;

    // Не даём таблице расти бесконечно (сканеры/боты с разных IP).
    if (g_ipRates.size() > 512)
    {
        for (auto it = g_ipRates.begin(); it != g_ipRates.end();)
        {
            if (nowMs - it->second.windowMs > 60000)
                it = g_ipRates.erase(it);
            else
                ++it;
        }
    }
    return true;
}

// Откат «штрафного» токена per-IP лимита при успешном JOIN. Лимит должен
// ограничивать pre-JOIN флуд/сканеры, а не легитимные переподключения
// (reconnect после разрыва, host migration): иначе серия быстрых
// connect+JOIN с одного IP упирается в лимит раньше, чем в реальную ёмкость
// сервера. Токен откатываем только если он был посчитан в текущем окне.
void RefundRateSlotOnJoin(const std::string& ip, uint64_t acceptedAtMs)
{
    std::lock_guard<std::mutex> lk(g_rateMutex);
    auto it = g_ipRates.find(ip);
    if (it == g_ipRates.end())
        return;
    if (it->second.windowMs != 0 && acceptedAtMs >= it->second.windowMs &&
        acceptedAtMs - it->second.windowMs < 1000 && it->second.count > 0)
    {
        --it->second.count;
    }
}

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD)
{
    g_running.store(false);
    return TRUE;
}
#else
void PosixSignalHandler(int)
{
    g_running.store(false);
}
#endif

std::atomic<uint64_t> g_nextSession{1};
// Таймаут молчащего клиента (мс). Настраивается вторым аргументом (сек).
uint64_t g_idleTimeoutMs = 30000;
// Host migration hardening: хост обязан присылать ИГРОВЫЕ сообщения
// (Frame/WorldState/actor и т.п.), а не только Ping от сетевого потока.
// Если игровой поток хоста завис, сессия должна мигрировать на гостя.
// Настраивается 4-м аргументом (мс), clamp 2..15 с; отдельно от idle timeout.
uint64_t g_hostGameTimeoutMs = 4000;
// task-175: начальный грейс хоста до первого game-thread сообщения. Загрузка
// уровня/меню может занять >4 с — в это время роли НЕ трогаем.
constexpr uint64_t kHostGameGraceMs = 15000;
// Test-only: если >0, сервер сам инициирует корректную остановку через N мс.
uint64_t g_testExitAfterMs = 0;

// Сообщения, которые приходят из ИГРОВОГО потока клиента (Frame идёт каждый
// tick coop::Tick; WorldState/actor — при изменениях). Ping/Pong, JOIN/LEAVE
// и серверные сообщения сюда не входят: они не доказывают живость игрового
// потока (Ping шлёт сетевой поток клиента раз в секунду).
bool IsGameThreadMessage(net::Msg m)
{
    switch (m)
    {
    case net::Msg::Frame:
    case net::Msg::Died:
    case net::Msg::Respawned:
    case net::Msg::WorldState:
    case net::Msg::Outfit:
    case net::Msg::Shot:
    case net::Msg::HorseOutfit:
    case net::Msg::WorldSnapshot:
    case net::Msg::ActionState:
    case net::Msg::PlayerVehicle:
    case net::Msg::MissionEvent:
    case net::Msg::MissionState:
    case net::Msg::MissionBarrier:
    case net::Msg::TrainSnapshot:
    case net::Msg::ActorSpawnBatch:
    case net::Msg::ActorFrames:
    case net::Msg::ActorDespawn:
    case net::Msg::ActorEvent:
    case net::Msg::ActorOutfit:
    case net::Msg::PlayerPresence: // task-175: клиент шлёт из игрового потока
    case net::Msg::ActorAppearance:
    // v20 P1 (§3.3): host activity; 0x23 (guest intent) сюда НЕ входит.
    case net::Msg::VehicleInteractionResult:
    case net::Msg::VehicleState:
        return true;
    default:
        return false;
    }
}

uint64_t NowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------- Логи ----------------
// Файл, куда дублируются строки лога (рядом с сервером в рабочей папке).
const std::string kLogFile = "server_console.log";

std::string Now()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    return buf;
}

void LogLine(const std::string& s)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::string line = "[" + Now() + "] " + s;
    std::cout << line << std::endl;
    // Дублируем ту же строку в файл (для диагностики без окна).
    std::ofstream f(kLogFile, std::ios::app);
    if (f) f << line << "\n";
}

// Редкие предупреждения (спам от кривых/враждебных клиентов не должен
// заливать лог и диск).
void LogWarn(const std::string& s)
{
    const uint64_t n = ++g_warnCounter;
    if (n <= 20 || (n % 100) == 0)
        LogLine(s);
}

// ---------------- Валидация payload'ов ----------------
// Практический лимит ника: защищает лог/players.txt от мусора и ':' в имени.
constexpr std::size_t kMaxNameLength = 32;
// Сколько сохранённых игроков держим в памяти/файле (защита от churn-DoS).
constexpr std::size_t kMaxSavedPlayers = 256;
// Сколько байт максимум читаем из players.txt при старте.
constexpr std::size_t kMaxSaveFileBytes = 128 * 1024;

bool IsFiniteCoord(float v)
{
    return std::isfinite(v) && std::fabs(v) < 1.0e6f;
}

bool IsValidName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxNameLength)
        return false;
    for (const unsigned char c : name)
    {
        if (c < 0x20 || c == 0x7F || c == ':')
            return false;
    }
    return true;
}

bool IsSaneFramePayload(const net::FramePayload& fp)
{
    return IsFiniteCoord(fp.x) && IsFiniteCoord(fp.y) && IsFiniteCoord(fp.z) &&
           IsFiniteCoord(fp.heading) && std::isfinite(fp.health) &&
           IsFiniteCoord(fp.aimX) && IsFiniteCoord(fp.aimY) &&
           IsFiniteCoord(fp.aimZ) && std::isfinite(fp.aimDist);
}

bool IsValidWorldSnapshot(const std::vector<uint8_t>& pay)
{
    if (pay.size() < sizeof(net::WorldSnapshotHeader))
        return false;
    net::WorldSnapshotHeader h{};
    std::memcpy(&h, pay.data(), sizeof(h));
    if (h.count > net::kMaxWorldEntities)
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::WorldEntity);
    if (pay.size() != need)
        return false;
    // Координаты должны быть конечными: NaN/Inf от хоста не ретранслируем.
    const uint8_t* data = pay.data() + sizeof(h);
    for (uint16_t i = 0; i < h.count; ++i)
    {
        net::WorldEntity e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e), sizeof(e));
        if (!IsFiniteCoord(e.x) || !IsFiniteCoord(e.y) ||
            !IsFiniteCoord(e.z) || !IsFiniteCoord(e.heading))
            return false;
    }
    return true;
}

bool IsValidTrainSnapshot(const std::vector<uint8_t>& pay)
{
    if (pay.size() < sizeof(net::TrainSnapshotHeader))
        return false;
    net::TrainSnapshotHeader h{};
    std::memcpy(&h, pay.data(), sizeof(h));
    if (h.carCount > net::kMaxTrainCars)
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.carCount) * sizeof(net::WorldEntity);
    if (pay.size() != need)
        return false;
    const uint8_t* data = pay.data() + sizeof(h);
    for (uint8_t i = 0; i < h.carCount; ++i)
    {
        net::WorldEntity e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e), sizeof(e));
        if (!IsFiniteCoord(e.x) || !IsFiniteCoord(e.y) ||
            !IsFiniteCoord(e.z) || !IsFiniteCoord(e.heading))
            return false;
    }
    return true;
}

// ---------------- E0: валидация actor-сообщений (v15) ----------------
// Проверяем точный размер header+count*record, count<=kMaxActors, epoch!=0,
// actorId!=0 и конечность координат (|coord| < 100000), чтобы не
// ретранслировать мусор от сломанного/враждебного хоста.
bool IsFiniteActorCoord(float v)
{
    return std::isfinite(v) && std::fabs(v) < 1.0e5f;
}

// ub-mode (task-165): точный размер + memcpy в одном месте. Валидаторы ниже
// проверяют только поля — проверка размера не дублируется в каждом.
template <typename T>
bool ReadExactPayload(const std::vector<uint8_t>& pay, T& out)
{
    if (pay.size() != sizeof(T)) return false;
    std::memcpy(&out, pay.data(), sizeof(out));
    return true;
}

bool IsSaneActorSpawnRecord(const net::ActorSpawn& a)
{
    return a.actorId != 0 &&
           IsFiniteActorCoord(a.x) && IsFiniteActorCoord(a.y) &&
           IsFiniteActorCoord(a.z) && IsFiniteActorCoord(a.heading);
}

bool IsSaneActorFrameRecord(const net::ActorFrame& f)
{
    return f.actorId != 0 &&
           IsFiniteActorCoord(f.x) && IsFiniteActorCoord(f.y) &&
           IsFiniteActorCoord(f.z) && IsFiniteActorCoord(f.heading);
}

bool IsValidActorSpawnBatch(const std::vector<uint8_t>& pay)
{
    if (pay.size() < sizeof(net::ActorSpawnBatchHeader))
        return false;
    net::ActorSpawnBatchHeader h{};
    std::memcpy(&h, pay.data(), sizeof(h));
    if (h.epoch == 0 || h.count > net::kMaxActors)
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::ActorSpawn);
    if (pay.size() != need)
        return false;
    const uint8_t* data = pay.data() + sizeof(h);
    uint32_t seen[net::kMaxActors] = {0};
    for (uint16_t i = 0; i < h.count; ++i)
    {
        net::ActorSpawn a{};
        std::memcpy(&a, data + static_cast<std::size_t>(i) * sizeof(a), sizeof(a));
        if (!IsSaneActorSpawnRecord(a))
            return false;
        // Дубли actorId в одном батче — признак битого/склеенного снапшота:
        // получатель не должен применить два разных состояния одного актора.
        for (uint16_t j = 0; j < i; ++j)
            if (seen[j] == a.actorId)
                return false;
        seen[i] = a.actorId;
    }
    return true;
}

bool IsValidActorFrames(const std::vector<uint8_t>& pay)
{
    if (pay.size() < sizeof(net::ActorFramesHeader))
        return false;
    net::ActorFramesHeader h{};
    std::memcpy(&h, pay.data(), sizeof(h));
    if (h.epoch == 0 || h.count > net::kMaxActors)
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::ActorFrame);
    if (pay.size() != need)
        return false;
    const uint8_t* data = pay.data() + sizeof(h);
    uint32_t seen[net::kMaxActors] = {0};
    for (uint16_t i = 0; i < h.count; ++i)
    {
        net::ActorFrame f{};
        std::memcpy(&f, data + static_cast<std::size_t>(i) * sizeof(f), sizeof(f));
        if (!IsSaneActorFrameRecord(f))
            return false;
        // Дубли actorId в одном батче кадров отбрасываем (см. spawn batch).
        for (uint16_t j = 0; j < i; ++j)
            if (seen[j] == f.actorId)
                return false;
        seen[i] = f.actorId;
    }
    return true;
}

bool IsValidActorDespawn(const std::vector<uint8_t>& pay)
{
    net::ActorDespawnPayload msg{};
    if (!ReadExactPayload(pay, msg)) return false;
    return msg.epoch != 0 && msg.actorId != 0;
}

bool IsValidActorEvent(const std::vector<uint8_t>& pay)
{
    net::ActorEventPayload ev{};
    if (!ReadExactPayload(pay, ev)) return false;
    return ev.epoch != 0 && ev.actorId != 0;
}

bool IsValidActorOutfit(const std::vector<uint8_t>& pay)
{
    net::ActorOutfitPayload msg{};
    if (!ReadExactPayload(pay, msg)) return false;
    return msg.epoch != 0 && msg.actorId != 0;
}

// E2c-b (v16): точная внешность NPC. Точный размер header+count*entry,
// count 1..kActorAppearanceMaxComponents (16), epoch/actorId != 0,
// reserved каждой записи = 0.
bool IsValidActorAppearance(const std::vector<uint8_t>& pay)
{
    if (pay.size() < sizeof(net::ActorAppearanceHeader))
        return false;
    net::ActorAppearanceHeader h{};
    std::memcpy(&h, pay.data(), sizeof(h));
    if (h.epoch == 0 || h.actorId == 0)
        return false;
    if (h.count < 1 || h.count > net::kActorAppearanceMaxComponents)
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::ActorAppearanceEntry);
    if (pay.size() != need)
        return false;
    const uint8_t* data = pay.data() + sizeof(h);
    for (uint8_t i = 0; i < h.count; ++i)
    {
        net::ActorAppearanceEntry e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e),
                    sizeof(e));
        if (e.reserved != 0)
            return false;
    }
    return true;
}

// E5 (v17): урон по NPC от гостя. Точный размер, epoch/actorId != 0,
// damage 1..kMaxActorDamage; хост-получатель решает сам.
bool IsValidActorDamage(const std::vector<uint8_t>& pay)
{
    net::ActorDamagePayload msg{};
    if (!ReadExactPayload(pay, msg)) return false;
    return msg.epoch != 0 && msg.actorId != 0 &&
           msg.damage >= net::kMinActorDamage &&
           msg.damage <= net::kMaxActorDamage;
}

// E6 (v18): UI-статус игрока. Точный размер 8, epoch != 0, только известные
// биты flags (неизвестные -> drop); flags=0 допустим («нет статуса»).
bool IsValidPlayerPresence(const std::vector<uint8_t>& pay)
{
    net::PlayerPresencePayload msg{};
    if (!ReadExactPayload(pay, msg)) return false;
    if (msg.epoch == 0)
        return false;
    return (msg.flags & ~net::kPlayerPresenceKnownFlags) == 0;
}

// E3d-c (v19): occupants транспорта. Точный размер 8+count*8, count 0..8,
// vehicleNetId != 0; каждая запись: actorId != 0, seat -1..8, carIndex <=16,
// harness <=5, только известные флаги.
bool IsValidVehicleOccupants(const std::vector<uint8_t>& pay)
{
    if (pay.size() < sizeof(net::VehicleOccupantsHeader))
        return false;
    net::VehicleOccupantsHeader h{};
    std::memcpy(&h, pay.data(), sizeof(h));
    if (h.vehicleNetId == 0)
        return false;
    if (h.count > net::kMaxVehicleOccupants)
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::VehicleOccupantEntry);
    if (pay.size() != need)
        return false;
    const uint8_t* data = pay.data() + sizeof(h);
    for (uint8_t i = 0; i < h.count; ++i)
    {
        net::VehicleOccupantEntry e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e),
                    sizeof(e));
        if (e.actorId == 0)
            return false;
        if (e.seat < net::kMinVehicleSeat || e.seat > net::kMaxVehicleSeat)
            return false;
        if ((e.flags & ~net::kVehicleOccupantKnownFlags) != 0)
            return false;
        if (e.carIndex > net::kMaxVehicleCarIndex)
            return false;
        if (e.harness > net::kMaxDraftHarness)
            return false;
    }
    return true;
}

// E3d-b (v19): поезд/состав v2. Точный размер 48, epoch/trainId != 0,
// carCount 1..kMaxTrainCars, speed -40..40, координаты конечны, известные флаги.
bool IsValidTrainSnapshotV2(const std::vector<uint8_t>& pay)
{
    net::TrainSnapshotV2Payload p{};
    if (!ReadExactPayload(pay, p)) return false;
    if (p.epoch == 0 || p.trainId == 0)
        return false;
    if (p.carCount < 1 || p.carCount > net::kMaxTrainCars)
        return false;
    if (!std::isfinite(p.speed) || p.speed < net::kMinTrainSpeed ||
        p.speed > net::kMaxTrainSpeed)
        return false;
    if (!IsFiniteActorCoord(p.x) || !IsFiniteActorCoord(p.y) ||
        !IsFiniteActorCoord(p.z) || !IsFiniteActorCoord(p.heading))
        return false;
    if ((p.flags & ~net::kTrainSnapshotV2KnownFlags) != 0)
        return false;
    return true;
}

bool IsValidActorResync(const std::vector<uint8_t>& pay)
{
    net::ActorResyncPayload msg{};
    if (!ReadExactPayload(pay, msg)) return false;
    return msg.epoch != 0;
}

// ---------------- Персистентность (players.txt) ----------------
// Формат строк: name:x:y:z:heading (ровно 4 числовых поля).
const std::string kSaveFile = "players.txt";

struct SavedPos
{
    std::string name;
    float x = 0, y = 0, z = 0, heading = 0;
};

// Строгий разбор числа: без исключений (std::stof раньше бросал и ронял
// весь сервер), с проверкой «всё число съедено» и конечности.
bool ParseFloatField(const char*& cur, const char* end, float& out)
{
    while (cur < end && (*cur == ' ' || *cur == '\t'))
        ++cur;
    if (cur >= end)
        return false;
    errno = 0;
    char* next = nullptr;
    const float v = std::strtof(cur, &next);
    if (next == cur || errno == ERANGE || !std::isfinite(v))
        return false;
    out = v;
    cur = next;
    return true;
}

// Строгий разбор строки сохранения: имя валидно, ровно 4 конечных числа,
// никаких лишних полей/хвостов. Битые строки пропускаются, а не роняют сервер.
bool ParseSavedLine(const std::string& line, SavedPos& out)
{
    const auto c1 = line.find(':');
    if (c1 == std::string::npos || c1 == 0 || c1 > kMaxNameLength)
        return false;
    out.name = line.substr(0, c1);
    if (!IsValidName(out.name))
        return false;

    const std::string rest = line.substr(c1 + 1);
    const char* cur = rest.c_str();
    const char* end = cur + rest.size();

    if (!ParseFloatField(cur, end, out.x)) return false;
    if (cur >= end || *cur != ':') return false;
    ++cur;
    if (!ParseFloatField(cur, end, out.y)) return false;
    if (cur >= end || *cur != ':') return false;
    ++cur;
    if (!ParseFloatField(cur, end, out.z)) return false;
    if (cur >= end || *cur != ':') return false;
    ++cur;
    if (!ParseFloatField(cur, end, out.heading)) return false;
    while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\r'))
        ++cur;
    return cur == end;
}

// Вызывать под g_mutex. Добавляет/обновляет запись, но не раздувает карту
// сверх kMaxSavedPlayers (защита от persisted-DoS через churn имён).
void UpdateLastPosLocked(const std::string& name, const OnlinePlayer& p)
{
    auto it = g_lastPos.find(name);
    if (it != g_lastPos.end())
    {
        it->second = p;
        return;
    }
    if (g_lastPos.size() < kMaxSavedPlayers)
        g_lastPos.emplace(name, p);
}

// Загрузить ВСЕ сохранённые позиции при старте. Иначе первый SaveAll после
// входа одного игрока затирает сохранения остальных (они ещё не в g_lastPos).
void LoadAllSaved()
{
    std::ifstream f(kSaveFile);
    if (!f)
        return;
    std::vector<SavedPos> loaded;
    std::string line;
    std::size_t totalBytes = 0;
    std::size_t bad = 0;
    while (std::getline(f, line))
    {
        totalBytes += line.size() + 2;
        if (totalBytes > kMaxSaveFileBytes || loaded.size() >= kMaxSavedPlayers)
        {
            LogLine("[!] players.txt: file truncated at limit, остальное проигнорировано");
            break;
        }
        if (line.empty())
            continue;
        SavedPos sp;
        if (ParseSavedLine(line, sp))
            loaded.push_back(sp);
        else
            ++bad;
    }
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (const auto& sp : loaded)
        {
            if (g_lastPos.size() >= kMaxSavedPlayers)
                break;
            if (g_lastPos.find(sp.name) != g_lastPos.end())
                continue;
            OnlinePlayer p;
            p.name = sp.name;
            p.x = sp.x;
            p.y = sp.y;
            p.z = sp.z;
            p.heading = sp.heading;
            p.hasLoc = true;
            g_lastPos.emplace(sp.name, p);
        }
    }
    if (bad > 0)
        LogLine("[!] players.txt: skipped " + std::to_string(bad) + " malformed line(s)");
}

void SaveAll()
{
    // Несколько клиентских потоков могут отключаться одновременно — без
    // сериализации они пишут один и тот же .tmp и могут потерять игроков.
    std::lock_guard<std::mutex> saveLk(g_saveMutex);

    // Копия под g_mutex, а запись на диск — БЕЗ g_mutex: иначе медленный
    // диск блокирует ретрансляцию кадров всем игрокам.
    std::vector<SavedPos> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        snapshot.reserve(g_lastPos.size());
        for (const auto& kv : g_lastPos)
        {
            const OnlinePlayer& p = kv.second;
            if (!p.hasLoc) continue;
            if (!IsValidName(p.name)) continue;
            if (!IsFiniteCoord(p.x) || !IsFiniteCoord(p.y) ||
                !IsFiniteCoord(p.z) || !IsFiniteCoord(p.heading))
                continue;
            SavedPos sp;
            sp.name = p.name;
            sp.x = p.x;
            sp.y = p.y;
            sp.z = p.z;
            sp.heading = p.heading;
            snapshot.push_back(std::move(sp));
        }
    }

    const std::string tmpFile = kSaveFile + ".tmp";
    {
        std::ofstream f(tmpFile, std::ios::trunc);
        if (!f)
        {
            LogLine("[!] players.txt: cannot open tmp file — save skipped");
            return;
        }
        f << std::setprecision(9);
        for (const auto& sp : snapshot)
            f << sp.name << ':' << sp.x << ':' << sp.y << ':' << sp.z
              << ':' << sp.heading << '\n';
        f.flush();
        if (!f.good())
        {
            f.close();
            std::remove(tmpFile.c_str());
            LogLine("[!] players.txt: write failed — keeping previous file");
            return;
        }
    }
    // Атомарная замена: битый/пустой players.txt не должен оставаться при
    // падении/обрыве записи.
#ifdef _WIN32
    if (!MoveFileExA(tmpFile.c_str(), kSaveFile.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        std::remove(tmpFile.c_str());
        LogLine("[!] players.txt atomic replace failed");
    }
#else
    if (std::rename(tmpFile.c_str(), kSaveFile.c_str()) != 0)
        std::remove(tmpFile.c_str());
#endif
}

// ---------------- Сборка сообщений ----------------
std::vector<uint8_t> MakeMsg(uint8_t type, const uint8_t* payload, uint32_t len)
{
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    out.push_back(type);
    if (payload && len) out.insert(out.end(), payload, payload + len);
    return out;
}

// PLAYER_JOINED / (для списка существующих) — name + model + (необязательно)
// последние/сохранённые координаты. Позиция даёт клиентам точку спавна педа.
std::vector<uint8_t> MakePlayerInfoMsg(uint8_t type, const std::string& name,
                                       unsigned long long model,
                                       bool hasPos, float x, float y, float z, float heading)
{
    std::vector<uint8_t> p;
    p.push_back(static_cast<uint8_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    for (int i = 0; i < 8; ++i)
        p.push_back(static_cast<uint8_t>((model >> (8 * i)) & 0xFF));
    p.push_back(hasPos ? 1 : 0);
    if (hasPos)
    {
        auto pushf = [&](float v) {
            uint32_t u;
            std::memcpy(&u, &v, 4);
            for (int i = 0; i < 4; ++i) p.push_back((uint8_t)(u >> (8 * i)));
        };
        pushf(x); pushf(y); pushf(z); pushf(heading);
    }
    return MakeMsg(type, p.data(), static_cast<uint32_t>(p.size()));
}

std::vector<uint8_t> MakeNameMsg(uint8_t type, const std::string& name)
{
    std::vector<uint8_t> p;
    p.push_back(static_cast<uint8_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    return MakeMsg(type, p.data(), static_cast<uint32_t>(p.size()));
}

// Отказ в JOIN (Spike 1): причина — один байт.
// Best-effort доставка: Send неблокирующий и мог вернуть WouldBlock, а
// Close освобождает очередь — без ограниченного drain клиент получал EOF без
// причины отказа и для VersionMismatch/BadName уходил в бесконечный reconnect.
// Ждём отправки не дольше kRejectFlushTimeoutMs и только на reject-пути, чтобы
// не блокировать остальные соединения.
constexpr uint64_t kRejectFlushTimeoutMs = 150;

void SendJoinRejectedAndClose(netio::Socket* sock, net::JoinRejectReason reason)
{
    const uint8_t r = static_cast<uint8_t>(reason);
    std::vector<uint8_t> msg = MakeMsg((uint8_t)net::Msg::JoinRejected, &r, 1);
    SendTracked(sock, msg.data(), static_cast<int>(msg.size()));

    const uint64_t deadlineMs = NowMs() + kRejectFlushTimeoutMs;
    while (sock->HasPending() && NowMs() < deadlineMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sock->Flush();
    }
    // Если за отведённое время кадр так и не ушёл (пир не читает) — закрываем:
    // зависание на медленном клиенте недопустимо; клиент получит EOF/реконнект.
    sock->Close();
}

// Разослать всем онлайн, кроме `except` (может быть nullptr).
void BroadcastToOthers(netio::Socket* except, const std::vector<uint8_t>& frame)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& kv : g_online)
    {
        netio::Socket* s = kv.second.sock;
        if (s && s != except)
            SendTracked(s, frame.data(), static_cast<int>(frame.size()));
    }
}

// ---------------- E0: host-only ретрансляция actor-сообщений ----------------
using PayloadValidator = bool (*)(const std::vector<uint8_t>&);

// 0x18-0x1C принимаем только от хоста (как WorldSnapshot): сообщения от
// не-хоста логируются и игнорируются; невалидные payload'ы дропаются;
// валидные уходят остальным байт-в-байт.
void RelayHostOnly(netio::Socket* sock, const std::string& name,
                   const std::vector<uint8_t>& pay, uint8_t type,
                   PayloadValidator validator, const char* what)
{
    bool senderIsHost = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_online.find(name);
        senderIsHost = it != g_online.end() && it->second.isHost;
    }
    if (!senderIsHost)
    {
        LogWarn("[!] " + name + ": " + what + " от не-хоста проигнорирован");
        return;
    }
    if (!validator(pay))
        return;
    BroadcastToOthers(sock, MakeMsg(type, pay.data(), static_cast<uint32_t>(pay.size())));
}


// ub-mode (task-165): host-only relay + per-client rate-limit. Раньше
// VehicleOccupants/TrainSnapshotV2 держали две копии этого блока.
void RelayHostOnlyRate(netio::Socket* sock, const std::string& name,
                       const std::vector<uint8_t>& pay, uint8_t type,
                       PayloadValidator validator,
                       RateWindow OnlinePlayer::*rate, int limit,
                       const char* what)
{
    if (!validator(pay))
        return;
    bool senderIsHost = false;
    bool allowed = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_online.find(name);
        if (it != g_online.end())
        {
            senderIsHost = it->second.isHost;
            if (senderIsHost)
                allowed = AllowRate(it->second.*rate, limit, NowMs());
        }
    }
    if (!senderIsHost)
        LogWarn("[!] " + name + ": " + what + " от не-хоста проигнорирован");
    else if (allowed)
        BroadcastToOthers(sock, MakeMsg(type, pay.data(),
                                         static_cast<uint32_t>(pay.size())));
}

// v20 P1 (§3.3): guest -> host unicast + per-client rate-limit (0x23).
// Хост-отправитель: warn+drop; нет хоста online — drop (сервер не эмулирует).
// ИНВАРИАНТ F1: чужой netio::Socket* нельзя сохранять вне g_mutex — cleanup
// другого потока удаляет сокет после Close. Поэтому и поиск хоста, и
// rate-check, и SendTracked выполняются в одной секции под lock (как в
// BroadcastToOthers / ActorDamage); наружу выходит только имя для LogWarn.
void RelayGuestToHostRate(netio::Socket* sock, const std::string& name,
                          const std::vector<uint8_t>& pay, uint8_t type,
                          PayloadValidator validator,
                          RateWindow OnlinePlayer::*rate, int limit,
                          const char* what)
{
    (void)sock;
    if (!validator(pay))
        return;
    const std::vector<uint8_t> msg = MakeMsg(
        type, pay.data(), static_cast<uint32_t>(pay.size()));
    bool senderIsHost = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_online.find(name);
        if (it != g_online.end())
        {
            senderIsHost = it->second.isHost;
            if (!senderIsHost)
            {
                for (auto& kv : g_online)
                {
                    if (!kv.second.isHost || !kv.second.sock)
                        continue;
                    if (AllowRate(it->second.*rate, limit, NowMs()))
                        SendTracked(kv.second.sock, msg.data(), (int)msg.size());
                    break;
                }
            }
        }
    }
    if (senderIsHost)
        LogWarn("[!] " + name + ": " + what + " от хоста проигнорирован");
    // Нет хоста / rate-limited: молча drop (клиент повторит по своей policy).
}


// ---------------- Таблица состояния ----------------
void PrintStatus()
{
    std::vector<OnlinePlayer> copy;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& kv : g_online) copy.push_back(kv.second);
    }

    std::ostringstream os;
    os << "=== Игроков в сети: " << copy.size()
       << " | TCP active=" << g_activeConnections.load()
       << " pre-join=" << g_preJoinConnections.load()
       << " peak=" << g_peakConnections.load()
       << " total=" << g_totalConnections.load()
       << " | rate-limited=" << g_rejectedRate.load()
       << " cap-rejected=" << g_rejectedCap.load()
       << " prejoin-rejected=" << g_rejectedPreJoin.load();
    {
        std::lock_guard<std::mutex> lk(g_rateMutex);
        os << " uniqueIPs=" << g_ipRates.size();
    }
    // Host liveness диагностика: сколько мс прошло с последнего ИГРОВОГО
    // сообщения хоста (Ping не учитывается; см. g_hostGameTimeoutMs).
    for (auto& p : copy)
    {
        if (p.isHost)
        {
            const uint64_t last = p.lastGameMs ? (NowMs() - p.lastGameMs) : 0;
            os << " host_game_last=" << last << "ms";
            break;
        }
    }
    os << " ===\n";
    // P0 (task-155): реальный трафик сервера + гистограмма входящих по типам
    // (frame/actor/world/outfit/presence/damage/trains/occupants/other).
    {
        auto tc = [](TrafficBucket b) -> unsigned long long {
            return static_cast<unsigned long long>(
                g_netTypeCounts[static_cast<int>(b)].load());
        };
        os << "  net: bytes_in=" << g_netBytesIn.load()
           << " bytes_out=" << g_netBytesOut.load()
           << " | frame=" << tc(TrafficBucket::Frame)
           << " actor=" << tc(TrafficBucket::Actor)
           << " world=" << tc(TrafficBucket::World)
           << " outfit=" << tc(TrafficBucket::Outfit)
           << " presence=" << tc(TrafficBucket::Presence)
           << " damage=" << tc(TrafficBucket::Damage)
           << " trains=" << tc(TrafficBucket::Trains)
           << " occupants=" << tc(TrafficBucket::Occupants)
           << " vehicle=" << tc(TrafficBucket::Vehicle)
           << " other=" << tc(TrafficBucket::Other) << "\n";
    }
    if (copy.empty())
    {
        os << "  (нет подключённых игроков)";
    }
    else
    {
        os << "  " << "ИМЯ" << "            " << "IP" << "                "
           << "КООРДИНАТЫ" << "                        " << "МОДЕЛЬ\n";
        for (auto& p : copy)
        {
            char pos[64];
            if (p.hasLoc)
                std::snprintf(pos, sizeof(pos), "%.1f %.1f %.1f", p.x, p.y, p.z);
            else
                std::snprintf(pos, sizeof(pos), "-");

            char line[256];
            std::snprintf(line, sizeof(line), "  %-15s %-18s %-32s 0x%08llX",
                          p.name.c_str(), p.ip.c_str(), pos,
                          (unsigned long long)(p.pedModel & 0xFFFFFFFFull));
            os << line << "\n";
        }
    }
    LogLine(os.str());
}

// ---------------- Статусный поток ----------------
void StatusThread()
{
    while (g_running.load())
    {
        for (int i = 0; i < 100 && g_running.load(); ++i) // ~10 сек
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!g_running.load()) break;
        PrintStatus();
    }
}

// ---------------- Обработка клиента ----------------
// ---------------- ub-mode (task-174): HandleClient phases ----------------
// HandleClient был 782 строки/CRAP 25440. Явная сессия ClientSession + фазы:
// read-loop, timeout/keepalive, dispatch, cleanup/save. Каждая фаза вызывается
// строго на месте прежнего кода; false = закрыть соединение и выйти (cleanup
// всё равно выполняется, как и прежний goto done).
struct ClientSession
{
    netio::Socket* sock = nullptr;
    std::string peerIp;
    std::string name;
    netio::FrameReader reader;
    bool registered = false;
    bool* preJoinCounted = nullptr;
    uint64_t connStartMs = 0;
    uint64_t lastRecvMs = 0;
    uint64_t acceptedAtMs = 0;
    uint8_t buf[4096] = {};
};

// Чтение из сокета (flushing хвоста, остановка сервера). false = break.
bool ReadSocketPhase(ClientSession& s)
{
    // Корректная остановка сервера (Ctrl+C): не ждём idle timeout.
    if (!g_running.load())
    {
        s.sock->Close();
        return false;
    }

    int n = 0;
    s.sock->Flush(); // дослать хвост из очереди (неблокирующие сокеты)
    const Result r = s.sock->Recv(s.buf, sizeof(s.buf), n);
    // Closed — сокет закрыт нами/другим потоком (например, Send обнаружил
    // обрыв). Раньше этот результат не обрабатывался, и обработчик крутился
    // в цикле вечно, не доходя до cleanup (зависший игрок в g_online).
    if (r == Result::NoData || r == Result::Error || r == Result::Closed)
    {
        s.sock->Close();
        return false;
    }
    if (r == Result::Ok)
    {
        s.lastRecvMs = NowMs();
        s.reader.Feed(s.buf, n);
    }
    else std::this_thread::sleep_for(std::chrono::milliseconds(1)); // не крутим CPU
    return true;
}

// Pre-JOIN deadline и idle timeout. false = close+break (логи как раньше).
bool CheckTimeoutPhase(ClientSession& s, uint64_t nowMs)
{
    // До JOIN — короткий дедлайн, чтобы «мёртвые» probe-сокеты не
    // занимали слоты до общего idle timeout.
    if (!s.registered && nowMs - s.connStartMs > kPreJoinTimeoutMs)
    {
        LogLine("[!] " + s.peerIp + ": нет JOIN за " +
                std::to_string(kPreJoinTimeoutMs / 1000) + " s — закрываю");
        s.sock->Close();
        return false;
    }
    if (nowMs - s.lastRecvMs > g_idleTimeoutMs)
    {
        LogLine("[!] " + s.peerIp + ": idle timeout (" +
                std::to_string(g_idleTimeoutMs / 1000) + " s), закрываю");
        s.sock->Close();
        return false;
    }
    return true;
}

// task-175: host game-timeout — МЯГКАЯ миграция, а НЕ закрытие сокета.
// Инварианты:
//  - до первого game-thread сообщения хост получает kHostGameGraceMs (15 с)
//    от JOIN/промоушена: загрузка/меню не должны дёргать роли;
//  - после первого game-thread сообщения действует обычный
//    g_hostGameTimeoutMs; Ping/Pong его НЕ продлевают;
//  - по таймауту сокет НЕ закрываем: снимаем isHost, повышаем второго,
//    обоим шлём HostChanged; старый хост остаётся обычным клиентом (idle/
//    connection cleanup закроет его только при реальном обрыве);
//  - если других игроков нет, роли не меняем (мигрировать некуда).
bool CheckHostGameTimeoutPhase(ClientSession& s, uint64_t nowMs)
{
    if (!s.registered || s.name.empty())
        return true;
    bool demote = false;
    uint64_t hostIdleMs = 0;
    std::string newHost;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_online.find(s.name);
        if (it == g_online.end() || it->second.sock != s.sock ||
            !it->second.isHost)
            return true;
        OnlinePlayer& h = it->second;
        if (!h.sawGameMsg)
        {
            if (nowMs - h.joinMs <= kHostGameGraceMs)
                return true; // загрузка/меню — ещё грейс
            demote = true;
            hostIdleMs = nowMs - h.joinMs;
        }
        else if (nowMs - h.lastGameMs > g_hostGameTimeoutMs)
        {
            demote = true;
            hostIdleMs = nowMs - h.lastGameMs;
        }
        if (!demote)
            return true;

        // Мигрировать можно только на другого игрока.
        auto other = g_online.end();
        for (auto nit = g_online.begin(); nit != g_online.end(); ++nit)
        {
            if (nit->first != s.name)
            {
                other = nit;
                break;
            }
        }
        if (other == g_online.end())
            return true; // некого повышать — хост остаётся хостом

        h.isHost = false;
        newHost = other->first;
        other->second.isHost = true;
        // Новому хосту — тот же грейс до первого game-thread сообщения.
        other->second.joinMs = nowMs;
        other->second.sawGameMsg = false;
        other->second.lastGameMs = nowMs;
        for (auto& kv : g_online)
        {
            net::HostChangedPayload hc{};
            hc.isHost = (kv.first == newHost) ? 1 : 0;
            const std::vector<uint8_t> msg = MakeMsg(
                (uint8_t)net::Msg::HostChanged,
                reinterpret_cast<const uint8_t*>(&hc),
                (uint32_t)sizeof(hc));
            if (kv.second.sock)
                SendTracked(kv.second.sock, msg.data(), (int)msg.size());
        }
    }
    if (demote)
    {
        LogLine("[!] host game-timeout (" + std::to_string(hostIdleMs) +
                " ms), promoting remaining");
        LogLine("[HOST] " + newHost + " повышен до хоста");
    }
    return true;
}

// ---------------- ub-mode (task-174): message dispatch phase ----------------
// Разбор всех входящих сообщений (pre-JOIN gate, host liveness, switch).
// false = соединение закрыто и нужно выйти (все прежние goto done пути уже
// вызвали Close); true = продолжаем read-loop.
// JOIN/handshake phase: разбор/валидация ника, версии, атомарная регистрация,
// JOIN_OK/PlayerJoined, RefundRateSlotOnJoin. false = соединение закрыто.
bool JoinRequestPhase(ClientSession& s, const std::vector<uint8_t>& pay)
{
    if (s.registered)
    {
        LogLine("[!] " + s.peerIp + ": повторный JOIN — соединение закрыто");
        s.sock->Close();
        return false;
    }

    // --- Разбор и валидация имени (до любых блокировок) ---
    if (pay.empty())
    {
        LogLine("[!] " + s.peerIp + " отклонён: пустой JOIN");
        SendJoinRejectedAndClose(s.sock, net::JoinRejectReason::BadName);
        return false;
    }
    const uint8_t nl = pay[0];
    if (nl == 0 || nl > kMaxNameLength || pay.size() < 1u + nl)
    {
        LogLine("[!] " + s.peerIp + " отклонён: некорректная длина ника");
        SendJoinRejectedAndClose(s.sock, net::JoinRejectReason::BadName);
        return false;
    }
    std::string candidate(reinterpret_cast<const char*>(pay.data() + 1), nl);
    if (!IsValidName(candidate))
    {
        LogLine("[!] " + s.peerIp + " отклонён: недопустимые символы в нике");
        SendJoinRejectedAndClose(s.sock, net::JoinRejectReason::BadName);
        return false;
    }

    // модель персонажа (может отсутствовать в старом клиенте)
    unsigned long long model = 0;
    const size_t off = 1u + nl;
    if (pay.size() >= off + 8)
    {
        for (int i = 0; i < 8; ++i)
            model |= (unsigned long long)pay[off + i] << (8 * i);
    }

    // Версия протокола обязательна: чужие байты не разбираем.
    uint16_t version = 0;
    if (pay.size() >= off + 10)
        version = static_cast<uint16_t>(pay[off + 8] |
                                        (pay[off + 9] << 8));
    if (version != net::kProtocolVersion)
    {
        LogLine("[!] " + s.peerIp + " отклонён: версия протокола " +
                std::to_string(version) + " != " +
                std::to_string(net::kProtocolVersion));
        SendJoinRejectedAndClose(s.sock, net::JoinRejectReason::VersionMismatch);
        return false;
    }

    // --- Атомарная регистрация + первые сообщения ---
    // Всё под одним g_mutex: проверки full/taken, вставка записи и
    // отправка JOIN_OK. Инвариант: ПЕРВОЕ сообщение новичку — всегда
    // JoinOk (или JoinRejected): другие потоки не могут вклиниться
    // и прислать PlayerJoined раньше JOIN_OK. Раньше проверка,
    // вставка и ответ были раздельными — два одновременных JOIN
    // могли дать двух хостов, третьего игрока или затирание записи
    // с тем же ником.
    bool full = false, taken = false, isHost = false, has = false;
    float sx = 0, sy = 0, sz = 0, shd = 0;
    uint64_t sid = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        full = g_online.size() >= static_cast<std::size_t>(net::kMaxPlayers);
        taken = !full && g_online.find(candidate) != g_online.end();
        if (!full && !taken)
        {
            const auto lp = g_lastPos.find(candidate);
            if (lp != g_lastPos.end() && lp->second.hasLoc)
            {
                has = true;
                sx = lp->second.x; sy = lp->second.y;
                sz = lp->second.z; shd = lp->second.heading;
            }
            isHost = g_online.empty();
            OnlinePlayer p;
            p.name = candidate;
            p.ip = s.peerIp;
            p.pedModel = model;
            p.sock = s.sock;
            p.isHost = isHost;
            p.sessionId = g_nextSession.fetch_add(1);
            p.lastGameMs = NowMs();
            p.joinMs = p.lastGameMs;
            p.sawGameMsg = false;
            if (has)
            {
                p.x = sx; p.y = sy; p.z = sz; p.heading = shd;
                p.hasLoc = true;
            }
            g_online[candidate] = p;
            UpdateLastPosLocked(candidate, p);
            s.name = candidate;
            sid = p.sessionId;
            s.registered = true;
            if ((*s.preJoinCounted))
            {
                --g_preJoinConnections;
                (*s.preJoinCounted) = false;
            }

            // 1) JOIN_OK — строго первым кадром новичку.
            std::vector<uint8_t> resp;
            uint8_t le[8];
            for (int i = 0; i < 8; ++i) le[i] = (uint8_t)(sid >> (8 * i));
            resp.insert(resp.end(), le, le + 8);
            resp.push_back(has ? 1 : 0);
            if (has)
            {
                auto pushf = [&](float v) {
                    uint32_t u;
                    std::memcpy(&u, &v, 4);
                    for (int i = 0; i < 4; ++i)
                        resp.push_back((uint8_t)(u >> (8 * i)));
                };
                pushf(sx); pushf(sy); pushf(sz); pushf(shd);
            }
            resp.push_back(isHost ? 1 : 0);
            const std::vector<uint8_t> ok = MakeMsg(
                (uint8_t)net::Msg::JoinOk, resp.data(), (uint32_t)resp.size());
            SendTracked(s.sock, ok.data(), (int)ok.size());

            // 2) Существующие игроки — новичку (уже после JOIN_OK).
            for (auto& kv : g_online)
            {
                if (kv.first == s.name) continue;
                const std::vector<uint8_t> info = MakePlayerInfoMsg(
                    (uint8_t)net::Msg::PlayerJoined,
                    kv.second.name, kv.second.pedModel,
                    kv.second.hasLoc, kv.second.x, kv.second.y,
                    kv.second.z, kv.second.heading);
                SendTracked(s.sock, info.data(), (int)info.size());
            }

            // 3) Остальным — что подключился новый (с его позицией).
            const std::vector<uint8_t> joinedMsg = MakePlayerInfoMsg(
                (uint8_t)net::Msg::PlayerJoined, s.name, model,
                has, sx, sy, sz, shd);
            for (auto& kv : g_online)
            {
                if (kv.first == s.name || !kv.second.sock) continue;
                SendTracked(kv.second.sock, joinedMsg.data(),
                            (int)joinedMsg.size());
            }
        }
    }

    if (!s.registered)
    {
        LogLine("[!] " + candidate + " отклонён: " +
                (full ? std::string("сервер полон")
                      : std::string("ник занят")));
        SendJoinRejectedAndClose(s.sock, full ? net::JoinRejectReason::ServerFull
                                            : net::JoinRejectReason::NameTaken);
        return false;
    }

    // JOIN удался — вернуть per-IP токен (см. RefundRateSlotOnJoin).
    RefundRateSlotOnJoin(s.peerIp, s.acceptedAtMs);

    LogLine("[+] " + s.name + " подключился (" + s.peerIp + ")" +
            (isHost ? " [HOST]" : " [GUEST]") +
            (has ? " — возврат, позиция восстановлена" : " — новый игрок"));
    return true;
}

// Leave/Died/Respawned phase. false = соединение закрыто (Leave).
bool LifecycleMessagesPhase(ClientSession& s,
                            const std::vector<uint8_t>& pay, net::Msg msg)
{
    switch (msg)
    {
        case net::Msg::Leave:
            LogLine("[-] " + s.name + " вышел");
            s.sock->Close();
            return false;
        case net::Msg::Died:
            LogLine("[x] " + s.name + " умер (игрок погиб в мире, " + s.peerIp + ")");
            // Ретранслируем остальным (с именем) — они покажут смерть
            // педа-призрака (убить, но оставить труп и снаряжение).
            if (s.registered && !s.name.empty())
                BroadcastToOthers(s.sock, MakeNameMsg((uint8_t)net::Msg::Died, s.name));
            break;
        case net::Msg::Respawned:
            LogLine("[+] " + s.name + " возродился после смерти");
            if (s.registered && !s.name.empty())
                BroadcastToOthers(s.sock, MakeNameMsg((uint8_t)net::Msg::Respawned, s.name));
            break;
    }
    return true;
}

void FrameMessagesPhase(ClientSession& s, const std::vector<uint8_t>& pay)
{
    switch (net::Msg::Frame)
    {
        case net::Msg::Frame:
        {
            if (pay.size() == sizeof(net::FramePayload))
            {
                net::FramePayload fp;
                std::memcpy(&fp, pay.data(), sizeof(fp));

                // NaN/Inf/абсурдные координаты не ретранслируем и не
                // сохраняем (иначе жертва получает SetEntityCoords(NaN),
                // а players.txt «заражается» навсегда).
                if (!IsSaneFramePayload(fp))
                {
                    LogWarn("[!] " + s.name +
                            ": FRAME с NaN/Inf/огромными координатами отброшен");
                    break;
                }

                // обновить координаты для таблицы и персистентности
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_online.find(s.name);
                    if (it != g_online.end())
                    {
                        it->second.x = fp.x; it->second.y = fp.y;
                        it->second.z = fp.z; it->second.heading = fp.heading;
                        it->second.hasLoc = true;
                        UpdateLastPosLocked(s.name, it->second);
                    }
                }

                // ретрансляция всем, кроме отправителя
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::Frame,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
    }
}

// World/mission relay phase (host-only/validated) — байт-в-байт.
void WorldRelayMessagesPhase(ClientSession& s,
                             const std::vector<uint8_t>& pay, net::Msg msg)
{
    switch (msg)
    {
        case net::Msg::WorldState:
        {
            // Время/погода канонические только у хоста (как WorldSnapshot).
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
            }
            if (!senderIsHost)
            {
                LogWarn("[!] " + s.name + ": WorldState от не-хоста проигнорирован");
                break;
            }
            if (pay.size() == sizeof(net::WorldState))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::WorldState,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::WorldSnapshot:
        {
            // P1: снапшот мира принимаем ТОЛЬКО от хоста сессии и
            // ретранслируем остальным (гостю). Гость не может подменить мир.
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
            }
            if (!senderIsHost)
            {
                LogWarn("[!] " + s.name + ": WorldSnapshot от не-хоста проигнорирован");
                break;
            }
            if (IsValidWorldSnapshot(pay))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::WorldSnapshot,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::Outfit:
        {
            // Компоненты одежды — ретранслируем остальным (как Frame).
            if (pay.size() == sizeof(net::OutfitState))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::Outfit,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::ActionState:
        {
            // P2: действие/сценарий своего персонажа — просто ретрансляция.
            if (pay.size() == sizeof(net::ActionStatePayload))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::ActionState,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::PlayerVehicle:
        {
            // P2: посадка в транспорт — ретрансляция остальным.
            if (pay.size() == sizeof(net::PlayerVehicleState))
            {
                BroadcastToOthers(s.sock, MakeMsg(
                    (uint8_t)net::Msg::PlayerVehicle,
                    pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::MissionEvent:
        {
            // P3: событие MissionData принимаем только от хоста.
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
            }
            if (!senderIsHost)
            {
                LogWarn("[!] " + s.name + ": MissionEvent от не-хоста проигнорирован");
                break;
            }
            if (pay.size() == sizeof(net::MissionEventPayload))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::MissionEvent,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::MissionState:
        {
            // P3: активная миссия — только от хоста.
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
            }
            if (!senderIsHost)
            {
                LogWarn("[!] " + s.name + ": MissionState от не-хоста проигнорирован");
                break;
            }
            if (pay.size() == sizeof(net::MissionStatePayload))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::MissionState,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::MissionBarrier:
        {
            // P3: направление фаз фиксировано: host шлёт Offer/StartWindow/
            // Abort, guest — Ready/Started. Иначе игнорируем.
            if (pay.size() != sizeof(net::MissionBarrierPayload))
                break;
            net::MissionBarrierPayload ev{};
            std::memcpy(&ev, pay.data(), sizeof(ev));
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
            }
            const bool hostPhase =
                coop::missionsbarrier::IsPhaseAllowedForRole(
                    coop::missionsbarrier::Role::Host, ev.phase);
            const bool guestPhase =
                coop::missionsbarrier::IsPhaseAllowedForRole(
                    coop::missionsbarrier::Role::Guest, ev.phase);
            const bool phaseAllowed =
                coop::missionsbarrier::IsKnownPhase(ev.phase) &&
                ((senderIsHost && hostPhase) || (!senderIsHost && guestPhase));
            if (!phaseAllowed)
            {
                LogWarn("[!] " + s.name + ": MissionBarrier phase=" +
                        std::to_string((unsigned)ev.phase) +
                        " не от той роли — проигнорирован");
                break;
            }
            BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::MissionBarrier,
                                            pay.data(), (uint32_t)pay.size()));
            break;
        }
        case net::Msg::TrainSnapshot:
        {
            // R&D: состав принимаем только от хоста и строго по формату.
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
            }
            if (!senderIsHost)
            {
                LogWarn("[!] " + s.name + ": TrainSnapshot от не-хоста проигнорирован");
                break;
            }
            if (IsValidTrainSnapshot(pay))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::TrainSnapshot,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        // ---------------- E0 (v15): actor-сообщения ----------------
    }
}

// Host-only actor relay phase (RelayHostOnly).
void ActorRelayMessagesPhase(ClientSession& s,
                             const std::vector<uint8_t>& pay, net::Msg msg)
{
    switch (msg)
    {
        case net::Msg::ActorSpawnBatch:
        {
            RelayHostOnly(s.sock, s.name, pay, (uint8_t)net::Msg::ActorSpawnBatch,
                          IsValidActorSpawnBatch, "ActorSpawnBatch");
            break;
        }
        case net::Msg::ActorFrames:
        {
            RelayHostOnly(s.sock, s.name, pay, (uint8_t)net::Msg::ActorFrames,
                          IsValidActorFrames, "ActorFrames");
            break;
        }
        case net::Msg::ActorDespawn:
        {
            RelayHostOnly(s.sock, s.name, pay, (uint8_t)net::Msg::ActorDespawn,
                          IsValidActorDespawn, "ActorDespawn");
            break;
        }
        case net::Msg::ActorEvent:
        {
            RelayHostOnly(s.sock, s.name, pay, (uint8_t)net::Msg::ActorEvent,
                          IsValidActorEvent, "ActorEvent");
            break;
        }
        case net::Msg::ActorOutfit:
        {
            RelayHostOnly(s.sock, s.name, pay, (uint8_t)net::Msg::ActorOutfit,
                          IsValidActorOutfit, "ActorOutfit");
            break;
        }
        case net::Msg::ActorAppearance:
        {
            // E2c-b (v16): точная внешность NPC — только хост -> гостям.
            RelayHostOnly(s.sock, s.name, pay,
                          (uint8_t)net::Msg::ActorAppearance,
                          IsValidActorAppearance, "ActorAppearance");
            break;
        }
    }
}

// v20 P1 (§3): 0x23 guest->host unicast; 0x24/0x26 host->guest relay.
void VehicleMessagesPhase(ClientSession& s,
                          const std::vector<uint8_t>& pay, net::Msg msg)
{
    switch (msg)
    {
        case net::Msg::VehicleInteraction:
            RelayGuestToHostRate(s.sock, s.name, pay,
                                 (uint8_t)net::Msg::VehicleInteraction,
                                 net::IsValidVehicleInteraction,
                                 &OnlinePlayer::interactionRate,
                                 kMaxVehicleInteractionPerSecond,
                                 "VehicleInteraction");
            break;
        case net::Msg::VehicleInteractionResult:
            RelayHostOnlyRate(s.sock, s.name, pay,
                              (uint8_t)net::Msg::VehicleInteractionResult,
                              net::IsValidVehicleInteractionResult,
                              &OnlinePlayer::interactionResultRate,
                              kMaxVehicleInteractionResultPerSecond,
                              "VehicleInteractionResult");
            break;
        case net::Msg::VehicleState:
            RelayHostOnlyRate(s.sock, s.name, pay,
                              (uint8_t)net::Msg::VehicleState,
                              net::IsValidVehicleState,
                              &OnlinePlayer::vehicleStateRate,
                              kMaxVehicleStatePerSecond,
                              "VehicleState");
            break;
        default:
            break;
    }
}

// Rate-limit/presence/traffic/misc phases (ActorDamage..Ping), без закрытия.
void MiscMessagesPhase(ClientSession& s,
                       const std::vector<uint8_t>& pay, net::Msg msg)
{
    switch (msg)
    {
        case net::Msg::ActorDamage:
        {
            // E5 (v17): урон по NPC — только гость -> сервер -> хосту.
            // Хост урон не запрашивает (он его применяет), гостям не
            // релеим; защита от флуда — не больше kMaxActorDamagePerSecond
            // запросов в секунду от клиента (лишние молча дропаются).
            if (!IsValidActorDamage(pay))
                break;
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                if (it != g_online.end())
                {
                    senderIsHost = it->second.isHost;
                    if (!senderIsHost &&
                        AllowRate(it->second.damageRate,
                                  kMaxActorDamagePerSecond, NowMs()))
                    {
                        {
                            const std::vector<uint8_t> msg = MakeMsg(
                                (uint8_t)net::Msg::ActorDamage,
                                pay.data(),
                                static_cast<uint32_t>(pay.size()));
                            for (auto& kv : g_online)
                            {
                                if (kv.second.isHost && kv.second.sock)
                                {
                                    SendTracked(kv.second.sock, msg.data(),
                                                (int)msg.size());
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (senderIsHost)
                LogWarn("[!] " + s.name + ": ActorDamage от хоста проигнорирован");
            break;
        }
        case net::Msg::PlayerPresence:
        {
            // E6 (v18): UI-статус — любой клиент -> сервер -> ДРУГОМУ
            // клиенту (без эха отправителю; при одном подключённом некому
            // релеить). Защита от флуда: <= kMaxPlayerPresencePerSecond/с.
            if (!IsValidPlayerPresence(pay))
                break;
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_online.find(s.name);
            if (it == g_online.end())
                break;
            if (!AllowRate(it->second.presenceRate,
                           kMaxPlayerPresencePerSecond, NowMs()))
                break; // rate-limited: молча дропаем
            const std::vector<uint8_t> msg = MakeMsg(
                (uint8_t)net::Msg::PlayerPresence,
                pay.data(), static_cast<uint32_t>(pay.size()));
            for (auto& kv : g_online)
            {
                if (kv.second.sock && kv.second.sock != s.sock)
                    SendTracked(kv.second.sock, msg.data(),
                                (int)msg.size());
            }
            break;
        }
        case net::Msg::VehicleOccupants:
        {
            // E3d-c (v19): occupants — только хост -> сервер -> гостю,
            // байт-в-байт; флуд-лимит kMaxVehicleOccupantsPerSecond/с.
            RelayHostOnlyRate(s.sock, s.name, pay,
                              (uint8_t)net::Msg::VehicleOccupants,
                              IsValidVehicleOccupants,
                              &OnlinePlayer::occupantsRate,
                              kMaxVehicleOccupantsPerSecond,
                              "VehicleOccupants");
            break;
        }
        case net::Msg::TrainSnapshotV2:
        {
            // E3d-b (v19): поезд v2 — только хост -> сервер -> гостю,
            // байт-в-байт; флуд-лимит kMaxTrainSnapshotV2PerSecond/с.
            RelayHostOnlyRate(s.sock, s.name, pay,
                              (uint8_t)net::Msg::TrainSnapshotV2,
                              IsValidTrainSnapshotV2,
                              &OnlinePlayer::trainV2Rate,
                              kMaxTrainSnapshotV2PerSecond,
                              "TrainSnapshotV2");
            break;
        }
        case net::Msg::ActorResync:
        {
            // Запрос resync — только от гостя и только хосту (не broadcast).
            if (!IsValidActorResync(pay))
                break;
            bool senderIsHost = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_online.find(s.name);
                senderIsHost = it != g_online.end() && it->second.isHost;
                if (!senderIsHost)
                {
                    const std::vector<uint8_t> msg = MakeMsg(
                        (uint8_t)net::Msg::ActorResync,
                        pay.data(), static_cast<uint32_t>(pay.size()));
                    for (auto& kv : g_online)
                    {
                        if (kv.second.isHost && kv.second.sock)
                        {
                            SendTracked(kv.second.sock, msg.data(),
                                        (int)msg.size());
                            break;
                        }
                    }
                }
            }
            if (senderIsHost)
                LogWarn("[!] " + s.name + ": ActorResync от хоста проигнорирован");
            break;
        }
        case net::Msg::HorseOutfit:
        {
            // Снаряжение лошади — ретранслируем остальным (как Outfit).
            if (pay.size() == sizeof(net::OutfitState))
            {
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::HorseOutfit,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::Shot:
        {
            // Событие выстрела — ретранслируем остальным.
            if (pay.size() == sizeof(net::ShotPayload))
            {
                net::ShotPayload sp{};
                std::memcpy(&sp, pay.data(), sizeof(sp));
                if (!IsFiniteCoord(sp.x) || !IsFiniteCoord(sp.y) ||
                    !IsFiniteCoord(sp.z))
                    break;
                BroadcastToOthers(s.sock, MakeMsg((uint8_t)net::Msg::Shot,
                                                pay.data(), (uint32_t)pay.size()));
            }
            break;
        }
        case net::Msg::Ping:
        {
            // Замер задержки: возвращаем ту же метку времени отправителю.
            // Строгий лимит размера: произвольные до 64 КБ payload'ы
            // больше не превращаются в очередь Pong'ов (анти-OOM).
            if (pay.size() <= kMaxPingEchoBytes)
            {
                std::vector<uint8_t> pong = MakeMsg((uint8_t)net::Msg::Pong,
                                                    pay.data(), (uint32_t)pay.size());
                SendTracked(s.sock, pong.data(), (int)pong.size());
            }
            break;
        }
        default:
            break;

    }
}

bool DispatchMessagesPhase(ClientSession& s, uint64_t nowMs)
{
    uint8_t type; std::vector<uint8_t> pay;
    while (s.reader.Poll(type, pay))
    {
        const net::Msg msg = static_cast<net::Msg>(type);
        // P0 (task-155): фактический входящий трафик (включая framing).
        CountInboundTraffic(msg, pay.size());

        // До JOIN разрешаем только JOIN и Ping. Остальное — закрываем:
        // клиент, не прошедший регистрацию, не должен ретранслировать мир.
        if (!s.registered && msg != net::Msg::Join && msg != net::Msg::Ping)
        {
            LogLine("[!] " + s.peerIp + ": сообщение type=" +
                    std::to_string((unsigned)type) + " до JOIN — соединение закрыто");
            s.sock->Close();
            return false;
        }

        // Host liveness: игровые сообщения (не Ping!) продлевают жизнь
        // хоста — по ним определяется, жив ли его игровой поток.
        if (s.registered && IsGameThreadMessage(msg))
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto git = g_online.find(s.name);
            if (git != g_online.end() && git->second.sock == s.sock)
            {
                git->second.lastGameMs = nowMs;
                git->second.sawGameMsg = true;
            }
        }

        switch (msg)
        {
        case net::Msg::Join:
            if (!JoinRequestPhase(s, pay)) return false;
            break;
        case net::Msg::Frame:
            FrameMessagesPhase(s, pay);
            break;
        case net::Msg::Leave:
        case net::Msg::Died:
        case net::Msg::Respawned:
            if (!LifecycleMessagesPhase(s, pay, msg)) return false;
            break;
        case net::Msg::WorldState:
        case net::Msg::WorldSnapshot:
        case net::Msg::Outfit:
        case net::Msg::ActionState:
        case net::Msg::PlayerVehicle:
        case net::Msg::MissionEvent:
        case net::Msg::MissionState:
        case net::Msg::MissionBarrier:
        case net::Msg::TrainSnapshot:
            WorldRelayMessagesPhase(s, pay, msg);
            break;
        case net::Msg::ActorSpawnBatch:
        case net::Msg::ActorFrames:
        case net::Msg::ActorDespawn:
        case net::Msg::ActorEvent:
        case net::Msg::ActorOutfit:
        case net::Msg::ActorAppearance:
            ActorRelayMessagesPhase(s, pay, msg);
            break;
        case net::Msg::ActorDamage:
        case net::Msg::PlayerPresence:
        case net::Msg::VehicleOccupants:
        case net::Msg::TrainSnapshotV2:
        case net::Msg::ActorResync:
        case net::Msg::HorseOutfit:
        case net::Msg::Shot:
        case net::Msg::Ping:
            MiscMessagesPhase(s, pay, msg);
            break;
        case net::Msg::VehicleInteraction:
        case net::Msg::VehicleInteractionResult:
        case net::Msg::VehicleState:
            VehicleMessagesPhase(s, pay, msg);
            break;
        case net::Msg::VehicleControlInput:
        case net::Msg::VehicleControlState:
            break; // RESERVED (P3): не реализуется в P1
        default:
            break;
        }
    }

    return true;
}



// Cleanup/disconnect: PlayerLeft, host promotion + HostChanged, SaveAll.
// Идемпотентен: erase только при совпадении sock, повторный вызов no-op.
void CleanupClient(ClientSession& s)
{
    if (s.registered && !s.name.empty())
    {
        bool erased = false;
        bool wasHost = false;
        std::string newHost;
        {
            // Всё под одним g_mutex: удаление, PlayerLeft, выбор нового хоста и
            // HostChanged. Раньше HostChanged отправлялся после unlock по
            // сырым Socket* — возможен use-after-free при одновременном обрыве.
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_online.find(s.name);
            if (it != g_online.end() && it->second.sock == s.sock)
            {
                wasHost = it->second.isHost;
                g_online.erase(it);
                erased = true;
            }
            if (erased)
            {
                // остальным — что игрок ушёл
                const std::vector<uint8_t> left =
                    MakeNameMsg((uint8_t)net::Msg::PlayerLeft, s.name);
                for (auto& kv : g_online)
                {
                    if (kv.second.sock)
                        SendTracked(kv.second.sock, left.data(),
                                    (int)left.size());
                }
                // Host migration: если ушёл хост и кто-то остался — повышаем
                // первого оставшегося и сообщаем каждому его роль (1 = хост).
                if (wasHost && !g_online.empty())
                {
                    auto nit = g_online.begin();
                    newHost = nit->first;
                    nit->second.isHost = true;
                    // Новому хосту — грейс-период: первый игровой пакет после
                    // промоушена может прийти не сразу.
                    nit->second.joinMs = NowMs();
                    nit->second.sawGameMsg = false;
                    nit->second.lastGameMs = nit->second.joinMs;
                    for (auto& kv : g_online)
                    {
                        net::HostChangedPayload h{};
                        h.isHost = (kv.first == newHost) ? 1 : 0;
                        auto msg = MakeMsg((uint8_t)net::Msg::HostChanged,
                                           reinterpret_cast<const uint8_t*>(&h),
                                           (uint32_t)sizeof(h));
                        if (kv.second.sock)
                            SendTracked(kv.second.sock, msg.data(),
                                        (int)msg.size());
                    }
                }
            }
        }
        if (erased)
        {
            LogLine("[-] " + s.name + " отключился (" + s.peerIp + ")");
            if (!newHost.empty())
                LogLine("[HOST] " + newHost + " повышен до хоста");
        }
        // SaveAll — только для реально зарегистрированного игрока: случайные
        // probe-подключения не должны перезаписывать players.txt.
        SaveAll();
    }
}

void HandleClient(netio::Socket* sock, const std::string& peerIp,
                  bool& preJoinCounted, uint64_t acceptedAtMs)
{
    ClientSession s;
    s.sock = sock;
    s.peerIp = peerIp;
    s.preJoinCounted = &preJoinCounted;
    s.acceptedAtMs = acceptedAtMs;
    s.connStartMs = NowMs();
    s.lastRecvMs = s.connStartMs;

    for (;;)
    {
        if (!ReadSocketPhase(s)) break;

        const uint64_t nowMs = NowMs();
        if (!CheckTimeoutPhase(s, nowMs)) break;

        if (!DispatchMessagesPhase(s, nowMs)) break;

        if (!CheckHostGameTimeoutPhase(s, nowMs)) break;
    }
done:
    CleanupClient(s);
}

} // namespace

namespace {

// ---------------- ub-mode (task-165): main-декомпозиция ----------------
// main раньше держал 209 строк (argv/config + socket + banner + accept-loop +
// shutdown). Каждая часть вынесена в маленькую функцию с явным контрактом;
// порядок и поведение прежние: argv/config → bind/listen → banner → timers →
// accept-loop → shutdown (drain + SaveAll + netio::Shutdown).

// Разбор argv: порт (1-й), idle sec (2-й), test-exit ms (3-й),
// host game-timeout ms (4-й, clamp 2..15 с). Один вход — дальше доверяем.
int ParsePortArg(int argc, char** argv)
{
    int port = argc > 1 ? std::atoi(argv[1]) : 7777;
    if (port <= 0 || port > 65535)
    {
        std::cerr << "invalid port, using 7777\n";
        port = 7777;
    }
    return port;
}

void ApplyTimeoutArgs(int argc, char** argv)
{
    if (argc > 2)
    {
        const int idleSec = std::atoi(argv[2]);
        if (idleSec > 0)
            g_idleTimeoutMs = static_cast<uint64_t>(idleSec) * 1000;
    }
    if (argc > 3)
    {
        const int exitMs = std::atoi(argv[3]);
        if (exitMs > 0)
            g_testExitAfterMs = static_cast<uint64_t>(exitMs);
    }
    if (argc > 4)
    {
        long long v = std::atoll(argv[4]);
        if (v <= 0) v = 4000;
        if (v < 2000) v = 2000;
        if (v > 15000) v = 15000;
        g_hostGameTimeoutMs = static_cast<uint64_t>(v);
    }
}

void InstallSignalHandlers()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, PosixSignalHandler);
    std::signal(SIGTERM, PosixSignalHandler);
#endif
}

void CloseSocketSafe(SOCKET s)
{
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

// Создать слушающий сокет (SO_REUSEADDR, bind, listen, non-blocking).
// INVALID_SOCKET = ошибка (причина уже в stderr), вызывающий выходит.
SOCKET OpenListenSocket(int port)
{
#ifdef _WIN32
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (ls == INVALID_SOCKET) { std::cerr << "socket() failed\n"; return INVALID_SOCKET; }

    int opt = 1;
#ifdef _WIN32
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

#ifdef _WIN32
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, 8) == SOCKET_ERROR)
    { std::cerr << "bind/listen failed\n"; closesocket(ls); return INVALID_SOCKET; }
#else
    if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) == -1 ||
        ::listen(ls, 8) == -1)
    { std::cerr << "bind/listen failed\n"; ::close(ls); return INVALID_SOCKET; }
#endif

    // Неблокирующий accept: позволяет корректно выйти по Ctrl+C и сделать SaveAll.
    netio::SetNonBlocking(ls);
    return ls;
}

void PrintStartupBanner(int port)
{
    std::cout << "RDR2 CO-OP — сервер-релей" << std::endl;
    std::cout << "Порт: " << port << ". Ожидание игроков..." << std::endl;
    std::cout << "Host game-timeout: " << (g_hostGameTimeoutMs / 1000) << " s"
              << " (idle timeout: " << (g_idleTimeoutMs / 1000) << " s)"
              << std::endl;
    std::cout << "(таблица состояния выводится каждые ~10 секунд)" << std::endl;
    std::cout << "(Ctrl+C — корректная остановка, players.txt сохраняется)" << std::endl;
}

void StartStatusThread()
{
    std::thread(StatusThread).detach();
}

void StartTestExitTimer()
{
    LogLine("[test] graceful exit after " +
            std::to_string(g_testExitAfterMs) + " ms");
    std::thread([ms = g_testExitAfterMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        g_running.store(false);
    }).detach();
}

// Проверка лимитов и запуск per-connection обработчика. Возвращает false,
// если соединение отклонено (сокет уже закрыт и причина залогирована).
bool AdmitAndStartClient(SOCKET c, const char* ipbuf, uint64_t acceptedAtMs)
{
    ++g_totalConnections;

    if (!AllowConnectionFrom(ipbuf, NowMs()))
    {
        ++g_rejectedRate;
        LogLine("[!] " + std::string(ipbuf) +
                ": per-IP rate limit (" +
                std::to_string(kMaxNewConnectionsPerIpPerSecond) +
                "/s) — соединение отклонено");
        CloseSocketSafe(c);
        return false;
    }

    if (g_activeConnections.load() >= kMaxActiveConnections)
    {
        ++g_rejectedCap;
        LogLine("[!] " + std::string(ipbuf) +
                ": лимит соединений (" +
                std::to_string(kMaxActiveConnections) + ") — отклонён");
        CloseSocketSafe(c);
        return false;
    }

    // Отдельный лимит на «ещё не JOIN-нутые» соединения: они не должны
    // вытеснять реальных игроков и жить до idle timeout.
    if (g_preJoinConnections.load() >= kMaxPreJoinConnections)
    {
        ++g_rejectedPreJoin;
        LogLine("[!] " + std::string(ipbuf) +
                ": лимит pre-JOIN соединений (" +
                std::to_string(kMaxPreJoinConnections) + ") — отклонён");
        CloseSocketSafe(c);
        return false;
    }

    const int activeNow = ++g_activeConnections;
    ++g_preJoinConnections;
    int peak = g_peakConnections.load();
    while (activeNow > peak &&
           !g_peakConnections.compare_exchange_weak(peak, activeNow))
    {
    }

    netio::Socket* cs = new netio::Socket(c);
    // Как и на клиенте: без Нагла + неблокирующий сокет.
    netio::SetNoDelay(c);
    netio::SetNonBlocking(c);
    std::thread([cs, ip = std::string(ipbuf), acceptedAtMs]() {
        bool preJoinCounted = true;
        try
        {
            HandleClient(cs, ip, preJoinCounted, acceptedAtMs);
        }
        catch (const std::exception& e)
        {
            // Ни одна ошибка обработчика не должна ронять весь сервер.
            LogLine(std::string("[!] handler exception: ") + e.what());
        }
        catch (...)
        {
            LogLine("[!] handler exception: unknown");
        }
        if (preJoinCounted)
            --g_preJoinConnections;
        delete cs;
        --g_activeConnections;
    }).detach();
    return true;
}

void AcceptLoop(SOCKET ls)
{
    while (g_running.load())
    {
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
#ifdef _WIN32
        SOCKET c = accept(ls, (sockaddr*)&peer, &peerLen);
#else
        SOCKET c = ::accept(ls, (sockaddr*)&peer, &peerLen);
#endif
        if (c == INVALID_SOCKET)
        {
            if (!g_running.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        char ipbuf[64] = "unknown";
        inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
        const uint64_t acceptedAtMs = NowMs();
        AdmitAndStartClient(c, ipbuf, acceptedAtMs);
    }
}

// Дождаться завершения клиентских обработчиков (они проверяют g_running и
// закрывают свои сокеты), затем финальный SaveAll.
void DrainClientThreadsAndSave()
{
    const uint64_t drainDeadlineMs = NowMs() + 2000;
    while (g_activeConnections.load() > 0 && NowMs() < drainDeadlineMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (g_activeConnections.load() > 0)
        LogLine("[!] shutdown: " + std::to_string(g_activeConnections.load()) +
                " обработчик(ов) не завершились за 2 с");

    SaveAll();
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // UTF-8 в консоли: иначе русский текст отображается «кракозябрами».
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (!netio::Init()) { std::cerr << "socket init failed\n"; return 1; }

    InstallSignalHandlers();
    LoadAllSaved();

    const int port = ParsePortArg(argc, argv);
    ApplyTimeoutArgs(argc, argv);
    const SOCKET ls = OpenListenSocket(port);
    if (ls == INVALID_SOCKET) return 1;

    PrintStartupBanner(port);
    StartStatusThread();
    if (g_testExitAfterMs > 0) StartTestExitTimer();

    AcceptLoop(ls);

    g_running.store(false);
    CloseSocketSafe(ls);

    DrainClientThreadsAndSave();
    netio::Shutdown();
    return 0;
}
