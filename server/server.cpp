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
struct OnlinePlayer {
    std::string name;
    std::string ip;
    uint64_t sessionId = 0;
    unsigned long long pedModel = 0;
    netio::Socket* sock = nullptr;
    float x = 0, y = 0, z = 0, heading = 0;
    bool hasLoc = false;
    bool isHost = false;   // первый подключившийся (его время/погода = канонические)
    // E5 (v17): окно rate-limit для ActorDamage (0x1F), защищено g_mutex.
    uint64_t damageWindowMs = 0;
    int damageCount = 0;
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
// Test-only: если >0, сервер сам инициирует корректную остановку через N мс.
uint64_t g_testExitAfterMs = 0;

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
    if (pay.size() != sizeof(net::ActorDespawnPayload))
        return false;
    net::ActorDespawnPayload msg{};
    std::memcpy(&msg, pay.data(), sizeof(msg));
    return msg.epoch != 0 && msg.actorId != 0;
}

bool IsValidActorEvent(const std::vector<uint8_t>& pay)
{
    if (pay.size() != sizeof(net::ActorEventPayload))
        return false;
    net::ActorEventPayload ev{};
    std::memcpy(&ev, pay.data(), sizeof(ev));
    return ev.epoch != 0 && ev.actorId != 0;
}

bool IsValidActorOutfit(const std::vector<uint8_t>& pay)
{
    if (pay.size() != sizeof(net::ActorOutfitPayload))
        return false;
    net::ActorOutfitPayload msg{};
    std::memcpy(&msg, pay.data(), sizeof(msg));
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
    if (pay.size() != sizeof(net::ActorDamagePayload))
        return false;
    net::ActorDamagePayload msg{};
    std::memcpy(&msg, pay.data(), sizeof(msg));
    return msg.epoch != 0 && msg.actorId != 0 &&
           msg.damage >= net::kMinActorDamage &&
           msg.damage <= net::kMaxActorDamage;
}

bool IsValidActorResync(const std::vector<uint8_t>& pay)
{
    if (pay.size() != sizeof(net::ActorResyncPayload))
        return false;
    net::ActorResyncPayload msg{};
    std::memcpy(&msg, pay.data(), sizeof(msg));
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
    sock->Send(msg.data(), static_cast<int>(msg.size()));

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
            s->Send(frame.data(), static_cast<int>(frame.size()));
    }
}

void BroadcastToAll(const std::vector<uint8_t>& frame)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& kv : g_online)
    {
        netio::Socket* s = kv.second.sock;
        if (s) s->Send(frame.data(), static_cast<int>(frame.size()));
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
    os << " ===\n";
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
void HandleClient(netio::Socket* sock, const std::string& peerIp,
                  bool& preJoinCounted, uint64_t acceptedAtMs)
{
    std::string name;
    netio::FrameReader reader;
    bool registered = false;
    const uint64_t connStartMs = NowMs();
    uint64_t lastRecvMs = connStartMs;

    uint8_t buf[4096];
    for (;;)
    {
        // Корректная остановка сервера (Ctrl+C): не ждём idle timeout.
        if (!g_running.load())
        {
            sock->Close();
            break;
        }

        int n = 0;
        sock->Flush(); // дослать хвост из очереди (неблокирующие сокеты)
        Result r = sock->Recv(buf, sizeof(buf), n);
        // Closed — сокет закрыт нами/другим потоком (например, Send обнаружил
        // обрыв и закрыл его). Раньше этот результат не обрабатывался, и
        // обработчик крутился в цикле вечно, не доходя до cleanup (зависший
        // игрок в g_online, слот не освобождается).
        if (r == Result::NoData || r == Result::Error || r == Result::Closed)
        {
            sock->Close();
            break;
        }
        if (r == Result::Ok)
        {
            lastRecvMs = NowMs();
            reader.Feed(buf, n);
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1)); // не крутим CPU

        const uint64_t nowMs = NowMs();
        // До JOIN — короткий дедлайн, чтобы «мёртвые» probe-сокеты не
        // занимали слоты до общего idle timeout.
        if (!registered && nowMs - connStartMs > kPreJoinTimeoutMs)
        {
            LogLine("[!] " + peerIp + ": нет JOIN за " +
                    std::to_string(kPreJoinTimeoutMs / 1000) + " s — закрываю");
            sock->Close();
            break;
        }
        if (nowMs - lastRecvMs > g_idleTimeoutMs)
        {
            LogLine("[!] " + peerIp + ": idle timeout (" +
                    std::to_string(g_idleTimeoutMs / 1000) + " s), закрываю");
            sock->Close();
            break;
        }

        uint8_t type; std::vector<uint8_t> pay;
        while (reader.Poll(type, pay))
        {
            const net::Msg msg = static_cast<net::Msg>(type);

            // До JOIN разрешаем только JOIN и Ping. Остальное — закрываем:
            // клиент, не прошедший регистрацию, не должен ретранслировать мир.
            if (!registered && msg != net::Msg::Join && msg != net::Msg::Ping)
            {
                LogLine("[!] " + peerIp + ": сообщение type=" +
                        std::to_string((unsigned)type) + " до JOIN — соединение закрыто");
                sock->Close();
                goto done;
            }

            switch (msg)
            {
            case net::Msg::Join:
            {
                if (registered)
                {
                    LogLine("[!] " + peerIp + ": повторный JOIN — соединение закрыто");
                    sock->Close();
                    goto done;
                }

                // --- Разбор и валидация имени (до любых блокировок) ---
                if (pay.empty())
                {
                    LogLine("[!] " + peerIp + " отклонён: пустой JOIN");
                    SendJoinRejectedAndClose(sock, net::JoinRejectReason::BadName);
                    goto done;
                }
                const uint8_t nl = pay[0];
                if (nl == 0 || nl > kMaxNameLength || pay.size() < 1u + nl)
                {
                    LogLine("[!] " + peerIp + " отклонён: некорректная длина ника");
                    SendJoinRejectedAndClose(sock, net::JoinRejectReason::BadName);
                    goto done;
                }
                std::string candidate(reinterpret_cast<const char*>(pay.data() + 1), nl);
                if (!IsValidName(candidate))
                {
                    LogLine("[!] " + peerIp + " отклонён: недопустимые символы в нике");
                    SendJoinRejectedAndClose(sock, net::JoinRejectReason::BadName);
                    goto done;
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
                    LogLine("[!] " + peerIp + " отклонён: версия протокола " +
                            std::to_string(version) + " != " +
                            std::to_string(net::kProtocolVersion));
                    SendJoinRejectedAndClose(sock, net::JoinRejectReason::VersionMismatch);
                    goto done;
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
                        p.ip = peerIp;
                        p.pedModel = model;
                        p.sock = sock;
                        p.isHost = isHost;
                        p.sessionId = g_nextSession.fetch_add(1);
                        if (has)
                        {
                            p.x = sx; p.y = sy; p.z = sz; p.heading = shd;
                            p.hasLoc = true;
                        }
                        g_online[candidate] = p;
                        UpdateLastPosLocked(candidate, p);
                        name = candidate;
                        sid = p.sessionId;
                        registered = true;
                        if (preJoinCounted)
                        {
                            --g_preJoinConnections;
                            preJoinCounted = false;
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
                        sock->Send(ok.data(), (int)ok.size());

                        // 2) Существующие игроки — новичку (уже после JOIN_OK).
                        for (auto& kv : g_online)
                        {
                            if (kv.first == name) continue;
                            const std::vector<uint8_t> info = MakePlayerInfoMsg(
                                (uint8_t)net::Msg::PlayerJoined,
                                kv.second.name, kv.second.pedModel,
                                kv.second.hasLoc, kv.second.x, kv.second.y,
                                kv.second.z, kv.second.heading);
                            sock->Send(info.data(), (int)info.size());
                        }

                        // 3) Остальным — что подключился новый (с его позицией).
                        const std::vector<uint8_t> joinedMsg = MakePlayerInfoMsg(
                            (uint8_t)net::Msg::PlayerJoined, name, model,
                            has, sx, sy, sz, shd);
                        for (auto& kv : g_online)
                        {
                            if (kv.first == name || !kv.second.sock) continue;
                            kv.second.sock->Send(joinedMsg.data(), (int)joinedMsg.size());
                        }
                    }
                }

                if (!registered)
                {
                    LogLine("[!] " + candidate + " отклонён: " +
                            (full ? std::string("сервер полон")
                                  : std::string("ник занят")));
                    SendJoinRejectedAndClose(sock, full ? net::JoinRejectReason::ServerFull
                                                        : net::JoinRejectReason::NameTaken);
                    goto done;
                }

                // JOIN удался — вернуть per-IP токен (см. RefundRateSlotOnJoin).
                RefundRateSlotOnJoin(peerIp, acceptedAtMs);

                LogLine("[+] " + name + " подключился (" + peerIp + ")" +
                        (isHost ? " [HOST]" : " [GUEST]") +
                        (has ? " — возврат, позиция восстановлена" : " — новый игрок"));
                break;
            }
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
                        LogWarn("[!] " + name +
                                ": FRAME с NaN/Inf/огромными координатами отброшен");
                        break;
                    }

                    // обновить координаты для таблицы и персистентности
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        auto it = g_online.find(name);
                        if (it != g_online.end())
                        {
                            it->second.x = fp.x; it->second.y = fp.y;
                            it->second.z = fp.z; it->second.heading = fp.heading;
                            it->second.hasLoc = true;
                            UpdateLastPosLocked(name, it->second);
                        }
                    }

                    // ретрансляция всем, кроме отправителя
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::Frame,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::Leave:
                LogLine("[-] " + name + " вышел");
                sock->Close();
                goto done;
            case net::Msg::Died:
                LogLine("[x] " + name + " умер (игрок погиб в мире, " + peerIp + ")");
                // Ретранслируем остальным (с именем) — они покажут смерть
                // педа-призрака (убить, но оставить труп и снаряжение).
                if (registered && !name.empty())
                    BroadcastToOthers(sock, MakeNameMsg((uint8_t)net::Msg::Died, name));
                break;
            case net::Msg::Respawned:
                LogLine("[+] " + name + " возродился после смерти");
                if (registered && !name.empty())
                    BroadcastToOthers(sock, MakeNameMsg((uint8_t)net::Msg::Respawned, name));
                break;
            case net::Msg::WorldState:
            {
                // Время/погода канонические только у хоста (как WorldSnapshot).
                bool senderIsHost = false;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_online.find(name);
                    senderIsHost = it != g_online.end() && it->second.isHost;
                }
                if (!senderIsHost)
                {
                    LogWarn("[!] " + name + ": WorldState от не-хоста проигнорирован");
                    break;
                }
                if (pay.size() == sizeof(net::WorldState))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::WorldState,
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
                    auto it = g_online.find(name);
                    senderIsHost = it != g_online.end() && it->second.isHost;
                }
                if (!senderIsHost)
                {
                    LogWarn("[!] " + name + ": WorldSnapshot от не-хоста проигнорирован");
                    break;
                }
                if (IsValidWorldSnapshot(pay))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::WorldSnapshot,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::Outfit:
            {
                // Компоненты одежды — ретранслируем остальным (как Frame).
                if (pay.size() == sizeof(net::OutfitState))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::Outfit,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::ActionState:
            {
                // P2: действие/сценарий своего персонажа — просто ретрансляция.
                if (pay.size() == sizeof(net::ActionStatePayload))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::ActionState,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::PlayerVehicle:
            {
                // P2: посадка в транспорт — ретрансляция остальным.
                if (pay.size() == sizeof(net::PlayerVehicleState))
                {
                    BroadcastToOthers(sock, MakeMsg(
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
                    auto it = g_online.find(name);
                    senderIsHost = it != g_online.end() && it->second.isHost;
                }
                if (!senderIsHost)
                {
                    LogWarn("[!] " + name + ": MissionEvent от не-хоста проигнорирован");
                    break;
                }
                if (pay.size() == sizeof(net::MissionEventPayload))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::MissionEvent,
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
                    auto it = g_online.find(name);
                    senderIsHost = it != g_online.end() && it->second.isHost;
                }
                if (!senderIsHost)
                {
                    LogWarn("[!] " + name + ": MissionState от не-хоста проигнорирован");
                    break;
                }
                if (pay.size() == sizeof(net::MissionStatePayload))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::MissionState,
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
                    auto it = g_online.find(name);
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
                    LogWarn("[!] " + name + ": MissionBarrier phase=" +
                            std::to_string((unsigned)ev.phase) +
                            " не от той роли — проигнорирован");
                    break;
                }
                BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::MissionBarrier,
                                                pay.data(), (uint32_t)pay.size()));
                break;
            }
            case net::Msg::TrainSnapshot:
            {
                // R&D: состав принимаем только от хоста и строго по формату.
                bool senderIsHost = false;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    auto it = g_online.find(name);
                    senderIsHost = it != g_online.end() && it->second.isHost;
                }
                if (!senderIsHost)
                {
                    LogWarn("[!] " + name + ": TrainSnapshot от не-хоста проигнорирован");
                    break;
                }
                if (IsValidTrainSnapshot(pay))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::TrainSnapshot,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            // ---------------- E0 (v15): actor-сообщения ----------------
            case net::Msg::ActorSpawnBatch:
            {
                RelayHostOnly(sock, name, pay, (uint8_t)net::Msg::ActorSpawnBatch,
                              IsValidActorSpawnBatch, "ActorSpawnBatch");
                break;
            }
            case net::Msg::ActorFrames:
            {
                RelayHostOnly(sock, name, pay, (uint8_t)net::Msg::ActorFrames,
                              IsValidActorFrames, "ActorFrames");
                break;
            }
            case net::Msg::ActorDespawn:
            {
                RelayHostOnly(sock, name, pay, (uint8_t)net::Msg::ActorDespawn,
                              IsValidActorDespawn, "ActorDespawn");
                break;
            }
            case net::Msg::ActorEvent:
            {
                RelayHostOnly(sock, name, pay, (uint8_t)net::Msg::ActorEvent,
                              IsValidActorEvent, "ActorEvent");
                break;
            }
            case net::Msg::ActorOutfit:
            {
                RelayHostOnly(sock, name, pay, (uint8_t)net::Msg::ActorOutfit,
                              IsValidActorOutfit, "ActorOutfit");
                break;
            }
            case net::Msg::ActorAppearance:
            {
                // E2c-b (v16): точная внешность NPC — только хост -> гостям.
                RelayHostOnly(sock, name, pay,
                              (uint8_t)net::Msg::ActorAppearance,
                              IsValidActorAppearance, "ActorAppearance");
                break;
            }
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
                    auto it = g_online.find(name);
                    if (it != g_online.end())
                    {
                        senderIsHost = it->second.isHost;
                        if (!senderIsHost)
                        {
                            const uint64_t now = NowMs();
                            if (it->second.damageWindowMs == 0 ||
                                now - it->second.damageWindowMs >= 1000)
                            {
                                it->second.damageWindowMs = now;
                                it->second.damageCount = 0;
                            }
                            const bool overLimit =
                                it->second.damageCount >= kMaxActorDamagePerSecond;
                            if (!overLimit)
                            {
                                ++it->second.damageCount;
                                const std::vector<uint8_t> msg = MakeMsg(
                                    (uint8_t)net::Msg::ActorDamage,
                                    pay.data(),
                                    static_cast<uint32_t>(pay.size()));
                                for (auto& kv : g_online)
                                {
                                    if (kv.second.isHost && kv.second.sock)
                                    {
                                        kv.second.sock->Send(msg.data(),
                                                             (int)msg.size());
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                if (senderIsHost)
                    LogWarn("[!] " + name + ": ActorDamage от хоста проигнорирован");
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
                    auto it = g_online.find(name);
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
                                kv.second.sock->Send(msg.data(), (int)msg.size());
                                break;
                            }
                        }
                    }
                }
                if (senderIsHost)
                    LogWarn("[!] " + name + ": ActorResync от хоста проигнорирован");
                break;
            }
            case net::Msg::HorseOutfit:
            {
                // Снаряжение лошади — ретранслируем остальным (как Outfit).
                if (pay.size() == sizeof(net::OutfitState))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::HorseOutfit,
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
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::Shot,
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
                    sock->Send(pong.data(), (int)pong.size());
                }
                break;
            }
            default:
                break;
            }
        }
    }
done:
    if (registered && !name.empty())
    {
        bool erased = false;
        bool wasHost = false;
        std::string newHost;
        {
            // Всё под одним g_mutex: удаление, PlayerLeft, выбор нового хоста и
            // HostChanged. Раньше HostChanged отправлялся после unlock по
            // сырым Socket* — возможен use-after-free при одновременном обрыве.
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_online.find(name);
            if (it != g_online.end() && it->second.sock == sock)
            {
                wasHost = it->second.isHost;
                g_online.erase(it);
                erased = true;
            }
            if (erased)
            {
                // остальным — что игрок ушёл
                const std::vector<uint8_t> left =
                    MakeNameMsg((uint8_t)net::Msg::PlayerLeft, name);
                for (auto& kv : g_online)
                {
                    if (kv.second.sock)
                        kv.second.sock->Send(left.data(), (int)left.size());
                }
                // Host migration: если ушёл хост и кто-то остался — повышаем
                // первого оставшегося и сообщаем каждому его роль (1 = хост).
                if (wasHost && !g_online.empty())
                {
                    auto nit = g_online.begin();
                    newHost = nit->first;
                    nit->second.isHost = true;
                    for (auto& kv : g_online)
                    {
                        net::HostChangedPayload h{};
                        h.isHost = (kv.first == newHost) ? 1 : 0;
                        auto msg = MakeMsg((uint8_t)net::Msg::HostChanged,
                                           reinterpret_cast<const uint8_t*>(&h),
                                           (uint32_t)sizeof(h));
                        if (kv.second.sock)
                            kv.second.sock->Send(msg.data(), (int)msg.size());
                    }
                }
            }
        }
        if (erased)
        {
            LogLine("[-] " + name + " отключился (" + peerIp + ")");
            if (!newHost.empty())
                LogLine("[HOST] " + newHost + " повышен до хоста");
        }
        // SaveAll — только для реально зарегистрированного игрока: случайные
        // probe-подключения не должны перезаписывать players.txt.
        SaveAll();
    }
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

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, PosixSignalHandler);
    std::signal(SIGTERM, PosixSignalHandler);
#endif

    LoadAllSaved();

    int port = argc > 1 ? std::atoi(argv[1]) : 7777;
    if (port <= 0 || port > 65535)
    {
        std::cerr << "invalid port, using 7777\n";
        port = 7777;
    }
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

#ifdef _WIN32
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (ls == INVALID_SOCKET) { std::cerr << "socket() failed\n"; return 1; }

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
    { std::cerr << "bind/listen failed\n"; closesocket(ls); return 1; }
#else
    if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) == -1 ||
        ::listen(ls, 8) == -1)
    { std::cerr << "bind/listen failed\n"; ::close(ls); return 1; }
#endif

    std::cout << "RDR2 CO-OP — сервер-релей" << std::endl;
    std::cout << "Порт: " << port << ". Ожидание игроков..." << std::endl;
    std::cout << "(таблица состояния выводится каждые ~10 секунд)" << std::endl;
    std::cout << "(Ctrl+C — корректная остановка, players.txt сохраняется)" << std::endl;

    // Неблокирующий accept: позволяет корректно выйти по Ctrl+C и сделать SaveAll.
    netio::SetNonBlocking(ls);

    // статусный поток
    std::thread st(StatusThread);
    st.detach();

    if (g_testExitAfterMs > 0)
    {
        LogLine("[test] graceful exit after " +
                std::to_string(g_testExitAfterMs) + " ms");
        std::thread([ms = g_testExitAfterMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            g_running.store(false);
        }).detach();
    }

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
        ++g_totalConnections;
        const uint64_t acceptedAtMs = NowMs();

        if (!AllowConnectionFrom(ipbuf, NowMs()))
        {
            ++g_rejectedRate;
            LogLine("[!] " + std::string(ipbuf) +
                    ": per-IP rate limit (" +
                    std::to_string(kMaxNewConnectionsPerIpPerSecond) +
                    "/s) — соединение отклонено");
            closesocket(c);
            continue;
        }

        if (g_activeConnections.load() >= kMaxActiveConnections)
        {
            ++g_rejectedCap;
            LogLine("[!] " + std::string(ipbuf) +
                    ": лимит соединений (" +
                    std::to_string(kMaxActiveConnections) + ") — отклонён");
            closesocket(c);
            continue;
        }

        // Отдельный лимит на «ещё не JOIN-нутые» соединения: они не должны
        // вытеснять реальных игроков и жить до idle timeout.
        if (g_preJoinConnections.load() >= kMaxPreJoinConnections)
        {
            ++g_rejectedPreJoin;
            LogLine("[!] " + std::string(ipbuf) +
                    ": лимит pre-JOIN соединений (" +
                    std::to_string(kMaxPreJoinConnections) + ") — отклонён");
            closesocket(c);
            continue;
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
    }

    g_running.store(false);
#ifdef _WIN32
    closesocket(ls);
#else
    ::close(ls);
#endif

    // Даём потокам клиентов завершиться (они проверяют g_running и закрывают
    // свои сокеты), затем финальный SaveAll. Без ожидания WSACleanup/выход
    // процесса могли произойти, пока обработчики ещё использовали сокеты.
    const uint64_t drainDeadlineMs = NowMs() + 2000;
    while (g_activeConnections.load() > 0 && NowMs() < drainDeadlineMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (g_activeConnections.load() > 0)
        LogLine("[!] shutdown: " + std::to_string(g_activeConnections.load()) +
                " обработчик(ов) не завершились за 2 с");

    SaveAll();
    netio::Shutdown();
    return 0;
}
