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
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
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

using netio::Result;

namespace {

// ---------------- Состояние сервера ----------------
struct OnlinePlayer {
    std::string name;
    std::string ip;
    unsigned long long pedModel = 0;
    netio::Socket* sock = nullptr;
    float x = 0, y = 0, z = 0, heading = 0;
    bool hasLoc = false;
    bool isHost = false;   // первый подключившийся (его время/погода = канонические)
};

std::mutex g_mutex;                       // защищает g_online и g_lastPos
std::mutex g_saveMutex;                   // сериализует SaveAll (tmp+rename)
std::mutex g_logMutex;                    // защищает вывод в консоль
std::map<std::string, OnlinePlayer> g_online;   // игроки онлайн (по имени)
std::map<std::string, OnlinePlayer> g_lastPos;  // последние позиции (персистентность)
std::atomic<bool> g_running{true};
std::atomic<int> g_activeConnections{0};
// Защита от thread/socket flood: больше этого числа TCP-соединений сервер
// не держит. Легитимных участников максимум двое; остальное — probe/rogue.
constexpr int kMaxActiveConnections = 32;
// Per-IP anti-flood: не больше N новых TCP-соединений в секунду с одного IP.
constexpr int kMaxNewConnectionsPerIpPerSecond = 12;
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
    return true;
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

uint64_t g_nextSession = 1;
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

// ---------------- Персистентность (players.txt) ----------------
// Формат строк: name:x:y:z:heading
const std::string kSaveFile = "players.txt";

// Практический лимит ника: защищает лог/players.txt от мусора и ':' в имени.
constexpr std::size_t kMaxNameLength = 32;

// Полная валидация WorldSnapshot: header + count*WorldEntity, count в лимите.
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
    return pay.size() == need;
}

bool LoadSaved(const std::string& name, float& x, float& y, float& z, float& heading)
{
    std::ifstream f(kSaveFile);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
    {
        auto c1 = line.find(':');
        if (c1 == std::string::npos) continue;
        if (line.substr(0, c1) != name) continue;
        std::string rest = line.substr(c1 + 1);
        float vals[4] = {0, 0, 0, 0};
        size_t first = 0;
        for (int k = 0; k < 4; ++k)
        {
            size_t c = rest.find(':', first);
            vals[k] = std::stof(rest.substr(first, c - first));
            first = (c == std::string::npos) ? rest.size() : c + 1;
        }
        x = vals[0]; y = vals[1]; z = vals[2]; heading = vals[3];
        return true;
    }
    return false;
}

// Загрузить ВСЕ сохранённые позиции при старте. Иначе первый SaveAll после
// входа одного игрока затирает сохранения остальных (они ещё не в g_lastPos).
void LoadAllSaved()
{
    std::ifstream f(kSaveFile);
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        const auto c1 = line.find(':');
        if (c1 == std::string::npos || c1 == 0) continue;
        const std::string savedName = line.substr(0, c1);
        std::string rest = line.substr(c1 + 1);
        float vals[4] = {0, 0, 0, 0};
        size_t first = 0;
        bool ok = true;
        for (int k = 0; k < 4; ++k)
        {
            const size_t c = rest.find(':', first);
            try
            {
                vals[k] = std::stof(rest.substr(first, c - first));
            }
            catch (...)
            {
                ok = false;
                break;
            }
            first = (c == std::string::npos) ? rest.size() : c + 1;
        }
        if (!ok) continue;

        OnlinePlayer p;
        p.name = savedName;
        p.x = vals[0]; p.y = vals[1]; p.z = vals[2]; p.heading = vals[3];
        p.hasLoc = true;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_lastPos[savedName] = p;
    }
}

void SaveAll()
{
    // Несколько клиентских потоков могут отключаться одновременно — без
    // сериализации они пишут один и тот же .tmp и могут потерять игроков.
    std::lock_guard<std::mutex> saveLk(g_saveMutex);
    const std::string tmpFile = kSaveFile + ".tmp";
    {
        std::ofstream f(tmpFile, std::ios::trunc);
        if (!f) return;
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& kv : g_lastPos)
        {
            const OnlinePlayer& p = kv.second;
            if (!p.hasLoc) continue;
            f << p.name << ":" << p.x << ":" << p.y << ":" << p.z << ":" << p.heading << "\n";
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
void SendJoinRejected(netio::Socket* sock, net::JoinRejectReason reason)
{
    const uint8_t r = static_cast<uint8_t>(reason);
    std::vector<uint8_t> msg = MakeMsg((uint8_t)net::Msg::JoinRejected, &r, 1);
    sock->Send(msg.data(), static_cast<int>(msg.size()));
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

// ---------------- Таблица состояния ----------------
void PrintStatus()
{
    std::vector<OnlinePlayer> copy;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& kv : g_online) copy.push_back(kv.second);
    }

    std::ostringstream os;
    os << "=== Игроков в сети: " << copy.size() << " ===\n";
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
void HandleClient(netio::Socket* sock, const std::string& peerIp)
{
    std::string name;
    netio::FrameReader reader;
    bool registered = false;
    uint64_t lastRecvMs = NowMs();

    uint8_t buf[4096];
    for (;;)
    {
        int n = 0;
        sock->Flush(); // дослать хвост из очереди (неблокирующие сокеты)
        Result r = sock->Recv(buf, sizeof(buf), n);
        if (r == Result::NoData || r == Result::Error) { sock->Close(); break; }
        if (r == Result::Ok)
        {
            lastRecvMs = NowMs();
            reader.Feed(buf, n);
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1)); // не крутим CPU

        if (NowMs() - lastRecvMs > g_idleTimeoutMs)
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
                if (pay.empty()) { sock->Close(); goto done; }
                uint8_t nl = pay[0];
                if (nl == 0 || nl > kMaxNameLength ||
                    pay.size() < 1u + nl) { sock->Close(); goto done; }
                name.assign(reinterpret_cast<char*>(pay.data() + 1), nl);
                if (name.find(':') != std::string::npos)
                {
                    SendJoinRejected(sock, net::JoinRejectReason::BadName);
                    LogLine("[!] " + peerIp + " отклонён: ':' в нике");
                    sock->Close();
                    goto done;
                }

                // модель персонажа (может отсутствовать в старом клиенте)
                unsigned long long model = 0;
                size_t off = 1u + nl;
                if (pay.size() >= off + 8)
                {
                    for (int i = 0; i < 8; ++i)
                        model |= (unsigned long long)pay[off + i] << (8 * i);
                }

                // Spike 1: версия протокола обязательна. Старые клиенты/серверы
                // несовместимы — отклоняем явно, а не разбираем чужие байты.
                uint16_t version = 0;
                if (pay.size() >= off + 10)
                    version = static_cast<uint16_t>(pay[off + 8] |
                                                    (pay[off + 9] << 8));
                if (version != net::kProtocolVersion)
                {
                    SendJoinRejected(sock, net::JoinRejectReason::VersionMismatch);
                    LogLine("[!] " + peerIp + " отклонён: версия протокола " +
                            std::to_string(version) + " != " +
                            std::to_string(net::kProtocolVersion));
                    sock->Close();
                    goto done;
                }
                if (name.empty())
                {
                    SendJoinRejected(sock, net::JoinRejectReason::BadName);
                    LogLine("[!] " + peerIp + " отклонён: пустой ник");
                    sock->Close();
                    goto done;
                }
                // Spike 1: сессия — хост + один гость; третий отклоняется.
                // (Дубликат ника тоже отклоняем, чтобы не плодить записи
                //  с одним сокетом — см. аудит.)
                {
                    bool full = false;
                    bool taken = false;
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        full = g_online.size() >= static_cast<std::size_t>(net::kMaxPlayers);
                        taken = g_online.find(name) != g_online.end();
                    }
                    if (full || taken)
                    {
                        SendJoinRejected(sock, full ? net::JoinRejectReason::ServerFull
                                                    : net::JoinRejectReason::NameTaken);
                        LogLine("[!] " + name + " отклонён: " +
                                (full ? std::string("сервер полон")
                                      : std::string("ник занят")));
                        sock->Close();
                        goto done;
                    }
                }

                float sx = 0, sy = 0, sz = 0, shd = 0;
                bool has = LoadSaved(name, sx, sy, sz, shd);

                // JOIN_OK: session_id + has_saved + [x,y,z,heading] + is_host
                std::vector<uint8_t> resp;
                uint8_t le[8];
                uint64_t sid = g_nextSession++;
                for (int i = 0; i < 8; ++i) le[i] = (uint8_t)(sid >> (8 * i));
                resp.insert(resp.end(), le, le + 8);
                resp.push_back(has ? 1 : 0);
                if (has)
                {
                    auto pushf = [&](float v) {
                        uint32_t u;
                        std::memcpy(&u, &v, 4);
                        for (int i = 0; i < 4; ++i) resp.push_back((uint8_t)(u >> (8 * i)));
                    };
                    pushf(sx); pushf(sy); pushf(sz); pushf(shd);
                }
                // is_host: первый подключившийся к пустому серверу — хост.
                bool isHost = false;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    isHost = g_online.empty();
                    resp.push_back(isHost ? 1 : 0);
                }
                std::vector<uint8_t> ok = MakeMsg((uint8_t)net::Msg::JoinOk, resp.data(),
                                                  (uint32_t)resp.size());
                sock->Send(ok.data(), (int)ok.size());

                // Существующие игроки — новичку (чтобы он их увидел),
                // с их последними известными координатами.
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    for (auto& kv : g_online)
                    {
                        std::vector<uint8_t> info =
                            MakePlayerInfoMsg((uint8_t)net::Msg::PlayerJoined,
                                              kv.second.name, kv.second.pedModel,
                                              kv.second.hasLoc,
                                              kv.second.x, kv.second.y,
                                              kv.second.z, kv.second.heading);
                        sock->Send(info.data(), (int)info.size());
                    }
                }

                // Регистрируем
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    OnlinePlayer p;
                    p.name = name;
                    p.ip = peerIp;
                    p.pedModel = model;
                    p.sock = sock;
                    p.isHost = isHost;
                    if (has) { p.x = sx; p.y = sy; p.z = sz; p.heading = shd; p.hasLoc = true; }
                    g_online[name] = p;
                    g_lastPos[name] = p;
                }

                // Остальным — что подключился новый, с его позицией
                // (сохранённой из players.txt или последней известной).
                {
                    bool hasPos = false;
                    float bx = 0, by = 0, bz = 0, bh = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        auto& np = g_online[name];
                        hasPos = np.hasLoc;
                        bx = np.x; by = np.y; bz = np.z; bh = np.heading;
                    }
                    BroadcastToOthers(sock,
                        MakePlayerInfoMsg((uint8_t)net::Msg::PlayerJoined, name, model,
                                          hasPos, bx, by, bz, bh));
                }

                LogLine("[+] " + name + " подключился (" + peerIp + ")" +
                        (isHost ? " [HOST]" : " [GUEST]") +
                        (has ? " — возврат, позиция восстановлена" : " — новый игрок"));
                registered = true;
                break;
            }
            case net::Msg::Frame:
            {
                if (pay.size() == sizeof(net::FramePayload))
                {
                    net::FramePayload fp;
                    std::memcpy(&fp, pay.data(), sizeof(fp));

                    // обновить координаты для таблицы и персистентности
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        auto it = g_online.find(name);
                        if (it != g_online.end())
                        {
                            it->second.x = fp.x; it->second.y = fp.y;
                            it->second.z = fp.z; it->second.heading = fp.heading;
                            it->second.hasLoc = true;
                            g_lastPos[name] = it->second;
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
                // Состояние мира от хоста — ретранслируем всем, кроме отправителя
                // (так же, как Frame). Сервер никак не интерпретирует payload.
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
                    LogLine("[!] " + name + ": WorldSnapshot от не-хоста проигнорирован");
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
                    LogLine("[!] " + name + ": MissionEvent от не-хоста проигнорирован");
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
                    LogLine("[!] " + name + ": MissionState от не-хоста проигнорирован");
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
                    ev.phase == net::kMissionBarrierOffer ||
                    ev.phase == net::kMissionBarrierStartWindow ||
                    ev.phase == net::kMissionBarrierAbort;
                const bool guestPhase =
                    ev.phase == net::kMissionBarrierReady ||
                    ev.phase == net::kMissionBarrierStarted;
                if ((senderIsHost && !hostPhase) || (!senderIsHost && !guestPhase))
                {
                    LogLine("[!] " + name + ": MissionBarrier phase=" +
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
                    LogLine("[!] " + name + ": TrainSnapshot от не-хоста проигнорирован");
                    break;
                }
                if (pay.size() >= sizeof(net::TrainSnapshotHeader))
                {
                    net::TrainSnapshotHeader h{};
                    std::memcpy(&h, pay.data(), sizeof(h));
                    const std::size_t need = sizeof(h) +
                        static_cast<std::size_t>(h.carCount) * sizeof(net::WorldEntity);
                    if (h.carCount <= net::kMaxTrainCars && pay.size() == need)
                    {
                        BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::TrainSnapshot,
                                                        pay.data(), (uint32_t)pay.size()));
                    }
                }
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
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::Shot,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::Ping:
            {
                // Замер задержки: возвращаем ту же метку времени отправителю.
                std::vector<uint8_t> pong = MakeMsg((uint8_t)net::Msg::Pong,
                                                    pay.data(), (uint32_t)pay.size());
                sock->Send(pong.data(), (int)pong.size());
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
        bool wasHost = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_online.find(name);
            wasHost = it != g_online.end() && it->second.isHost;
        }
        // остальным — что игрок ушёл
        BroadcastToOthers(sock, MakeNameMsg((uint8_t)net::Msg::PlayerLeft, name));
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_online.erase(name);
        }
        LogLine("[-] " + name + " отключился (" + peerIp + ")");

        // Host migration: если ушёл хост и кто-то остался — повышаем первого
        // оставшегося и сообщаем каждому его роль (1 = хост).
        if (wasHost)
        {
            std::string newHost;
            std::vector<std::pair<netio::Socket*, bool>> targets;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                if (!g_online.empty())
                {
                    newHost = g_online.begin()->first;
                    g_online.begin()->second.isHost = true;
                }
                for (auto& kv : g_online)
                    targets.emplace_back(kv.second.sock, kv.first == newHost);
            }
            if (!newHost.empty())
            {
                LogLine("[HOST] " + newHost + " повышен до хоста");
                for (auto& t : targets)
                {
                    net::HostChangedPayload h{};
                    h.isHost = t.second ? 1 : 0;
                    auto msg = MakeMsg((uint8_t)net::Msg::HostChanged,
                                       reinterpret_cast<const uint8_t*>(&h),
                                       (uint32_t)sizeof(h));
                    if (t.first) t.first->Send(msg.data(), (int)msg.size());
                }
            }
        }
    }
    // SaveAll только для реально зарегистрированного игрока: случайные
    // probe-подключения не должны перезаписывать players.txt пустым файлом.
    if (registered)
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

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, PosixSignalHandler);
    std::signal(SIGTERM, PosixSignalHandler);
#endif

    LoadAllSaved();

    // Русский вывод в консоли Windows (UTF-8 вместо крякозябров).
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    int port = argc > 1 ? std::atoi(argv[1]) : 7777;
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

        if (!AllowConnectionFrom(ipbuf, NowMs()))
        {
            LogLine("[!] " + std::string(ipbuf) +
                    ": per-IP rate limit (" +
                    std::to_string(kMaxNewConnectionsPerIpPerSecond) +
                    "/s) — соединение отклонено");
            closesocket(c);
            continue;
        }

        if (g_activeConnections.load() >= kMaxActiveConnections)
        {
            LogLine("[!] " + std::string(ipbuf) +
                    ": лимит соединений (" +
                    std::to_string(kMaxActiveConnections) + ") — отклонён");
            closesocket(c);
            continue;
        }
        ++g_activeConnections;

        netio::Socket* cs = new netio::Socket(c);
        // Как и на клиенте: без Нагла + неблокирующий сокет.
        netio::SetNoDelay(c);
        netio::SetNonBlocking(c);
        std::thread([cs, ip = std::string(ipbuf)]() {
            HandleClient(cs, ip);
            delete cs;
            --g_activeConnections;
        }).detach();
    }

    g_running.store(false);
    SaveAll();
#ifdef _WIN32
    closesocket(ls);
#else
    ::close(ls);
#endif
    netio::Shutdown();
    return 0;
}