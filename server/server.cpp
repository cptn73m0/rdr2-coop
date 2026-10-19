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
std::mutex g_logMutex;                    // защищает вывод в консоль
std::map<std::string, OnlinePlayer> g_online;   // игроки онлайн (по имени)
std::map<std::string, OnlinePlayer> g_lastPos;  // последние позиции (персистентность)
std::atomic<bool> g_running{true};
uint64_t g_nextSession = 1;

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

void SaveAll()
{
    std::ofstream f(kSaveFile);
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& kv : g_lastPos)
    {
        const OnlinePlayer& p = kv.second;
        if (!p.hasLoc) continue;
        f << p.name << ":" << p.x << ":" << p.y << ":" << p.z << ":" << p.heading << "\n";
    }
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

    uint8_t buf[4096];
    for (;;)
    {
        int n = 0;
        sock->Flush(); // дослать хвост из очереди (неблокирующие сокеты)
        Result r = sock->Recv(buf, sizeof(buf), n);
        if (r == Result::NoData || r == Result::Error) { sock->Close(); break; }
        if (r == Result::Ok) reader.Feed(buf, n);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1)); // не крутим CPU

        uint8_t type; std::vector<uint8_t> pay;
        while (reader.Poll(type, pay))
        {
            switch (static_cast<net::Msg>(type))
            {
            case net::Msg::Join:
            {
                if (pay.empty()) { sock->Close(); goto done; }
                uint8_t nl = pay[0];
                if (pay.size() < 1u + nl) { sock->Close(); goto done; }
                name.assign(reinterpret_cast<char*>(pay.data() + 1), nl);

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
                if (pay.size() >= sizeof(net::FramePayload))
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
                if (pay.size() >= sizeof(net::WorldState))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::WorldState,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::Outfit:
            {
                // Компоненты одежды — ретранслируем остальным (как Frame).
                if (pay.size() >= sizeof(net::OutfitState))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::Outfit,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::HorseOutfit:
            {
                // Снаряжение лошади — ретранслируем остальным (как Outfit).
                if (pay.size() >= sizeof(net::OutfitState))
                {
                    BroadcastToOthers(sock, MakeMsg((uint8_t)net::Msg::HorseOutfit,
                                                    pay.data(), (uint32_t)pay.size()));
                }
                break;
            }
            case net::Msg::Shot:
            {
                // Событие выстрела — ретранслируем остальным.
                if (pay.size() >= sizeof(net::ShotPayload))
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
        // остальным — что игрок ушёл
        BroadcastToOthers(sock, MakeNameMsg((uint8_t)net::Msg::PlayerLeft, name));
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_online.erase(name);
        }
        LogLine("[-] " + name + " отключился (" + peerIp + ")");
    }
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

    // Русский вывод в консоли Windows (UTF-8 вместо крякозябров).
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    int port = argc > 1 ? std::atoi(argv[1]) : 7777;

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

    // статусный поток
    std::thread st(StatusThread);
    st.detach();

    for (;;)
    {
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
#ifdef _WIN32
        SOCKET c = accept(ls, (sockaddr*)&peer, &peerLen);
        if (c == INVALID_SOCKET) break;
#else
        SOCKET c = ::accept(ls, (sockaddr*)&peer, &peerLen);
        if (c == INVALID_SOCKET) break;
#endif
        char ipbuf[64] = "unknown";
        inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));

        netio::Socket* cs = new netio::Socket(c);
        // Как и на клиенте: без Нагла + неблокирующий сокет.
        netio::SetNoDelay(c);
        netio::SetNonBlocking(c);
        std::thread([cs, ip = std::string(ipbuf)]() {
            HandleClient(cs, ip);
            delete cs;
        }).detach();
    }

    g_running.store(false);
    SaveAll();
    netio::Shutdown();
    return 0;
}