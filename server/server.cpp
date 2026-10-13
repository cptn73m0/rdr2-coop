// RDR2 CO-OP вЂ” СЃРµСЂРІРµСЂ-СЂРµР»РµР№ (Р±Р»РѕРєРёСЂСѓСЋС‰РёР№, thread-per-client).
// РљСЂРѕСЃСЃРїР»Р°С‚С„РѕСЂРјРµРЅРЅРѕРµ РєРѕРЅСЃРѕР»СЊРЅРѕРµ РїСЂРёР»РѕР¶РµРЅРёРµ Р±РµР· GUI. РЎР»СѓС€Р°РµС‚ TCP-РїРѕСЂС‚,
// РїСЂРёРЅРёРјР°РµС‚ РєР»РёРµРЅС‚РѕРІ (ASI-РїР»Р°РіРёРЅС‹), РѕР±СЂР°Р±Р°С‚С‹РІР°РµС‚ JOIN/FRAME/LEAVE Рё
// РїРµСЂРµСЃС‹Р»Р°РµС‚ FRAME РѕСЃС‚Р°Р»СЊРЅС‹Рј РєР»РёРµРЅС‚Р°Рј. РҐСЂР°РЅРёС‚ last-position РІ players.json.
//
// РџСЂРѕРµРєС‚ С‚СЂРµР±СѓРµС‚ РєСЂРѕСЃСЃРїР»Р°С‚С„РѕСЂРјРµРЅРЅРѕРіРѕ СЃРµСЂРІРµСЂР° (Windows Рё Linux) вЂ” РёСЃРїРѕР»СЊР·СѓРµРј
// РёСЃРєР»СЋС‡РёС‚РµР»СЊРЅРѕ std::thread + СЃРёСЃС‚РµРјРЅС‹Рµ СЃРѕРєРµС‚С‹, Р±РµР· РѕРєРѕРЅРЅС‹С… Р·Р°РІРёСЃРёРјРѕСЃС‚РµР№,
// РїРѕСЌС‚РѕРјСѓ С‚РѕС‚ Р¶Рµ РєРѕРґ РїРѕР·Р¶Рµ СЃРѕР±РµСЂС‘С‚СЃСЏ РїРѕРґ Linux Р±РµР· РёР·РјРµРЅРµРЅРёР№.

#include "common.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "net/frame.h"
#include "net/protocol.h"
#include "net/socket.h"

using netio::Result;

namespace {

// Р РµРґР°РєС‚РёСЂСѓРµРјРѕРµ СЃРѕСЃС‚РѕСЏРЅРёРµ (Р·Р°С‰РёС‰РµРЅРѕ mutex).
struct PlayerView {
    std::string name;
    float x=0, y=0, z=0, heading=0;
    bool hasLoc = false;
};

std::mutex g_mutex;
std::map<std::string, PlayerView> g_players; // РїРѕ РёРјРµРЅРё
std::set<netio::Socket*> g_sockets;          // Р¶РёРІС‹Рµ СЃРѕРєРµС‚С‹ РґР»СЏ СЂР°СЃСЃС‹Р»РєРё

uint64_t g_nextSession = 1;

// --- players.json (РїРѕСЃР»РµРґРЅСЏСЏ РёР·РІРµСЃС‚РЅР°СЏ РїРѕР·РёС†РёСЏ РЅР° РґРёСЃРє) ---------------
// Р¤РѕСЂРјР°С‚ СЃС‚СЂРѕРє: "name:x:y:z:heading" РїРѕ СЃС‚СЂРѕРєР°Рј (РїСЂРѕСЃС‚РµР№С€РёР№, Р±РµР· JSON).
const std::string kSaveFile = "players.txt";

bool LoadSaved(const std::string& name, PlayerView& out)
{
    std::ifstream f(kSaveFile);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line))
    {
        // name:x:y:z:heading
        auto c1 = line.find(':'); if (c1 == std::string::npos) continue;
        std::string n = line.substr(0, c1);
        if (n != name) continue;
        std::string rest = line.substr(c1 + 1);
        int first = 0, i = 0;
        float vals[4] = {0,0,0,0};
        for (int k = 0; k < 4; ++k)
        {
            std::size_t c = rest.find(':', first);
            vals[k] = std::stof(rest.substr(first, c - first));
            first = c == std::string::npos ? (int)rest.size() : (int)c + 1;
        }
        out = {name, vals[0], vals[1], vals[2], vals[3], true};
        return true;
    }
    return false;
}

void SaveAll()
{
    std::ofstream f(kSaveFile);
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& kv : g_players)
    {
        if (!kv.second.hasLoc) continue;
        f << kv.first << ":" << kv.second.x << ":"
          << kv.second.y << ":" << kv.second.z << ":"
          << kv.second.heading << "\n";
    }
}

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

std::vector<uint8_t> MakeNameMsg(uint8_t type, const std::string& name)
{
    std::vector<uint8_t> p;
    p.push_back(static_cast<uint8_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    return MakeMsg(type, p.data(), static_cast<uint32_t>(p.size()));
}

// Broadcast РёРіСЂРѕРєСѓ СЃ РёРјРµРЅРµРј `except` (РїСѓСЃС‚РѕРµ вЂ” РІСЃРµРј), РєР°РґСЂ FRAME.
void RelayFrame(const std::string& from, const uint8_t* payload, uint32_t len)
{
    std::vector<uint8_t> frame = MakeMsg(static_cast<uint8_t>(net::Msg::Frame), payload, len);
    std::lock_guard<std::mutex> lk(g_mutex);
    (void)from;
    for (auto* s : g_sockets)
        s->Send(frame.data(), static_cast<int>(frame.size()));
}

void HandleClient(netio::Socket* sock, const std::string& peer)
{
    std::string name;
    uint64_t sessionId = 0;
    netio::FrameReader reader;

    // JOIN РѕР¶РёРґР°РµРј РїРµСЂРІС‹Рј СЃРѕРѕР±С‰РµРЅРёРµРј.
    uint8_t buf[4096];
    for (;;)
    {
        int n = 0;
        Result r = sock->Recv(buf, sizeof(buf), n);
        if (r == Result::NoData || r == Result::Error) { sock->Close(); break; }
        if (r == Result::Ok) reader.Feed(buf, n);

        uint8_t type; std::vector<uint8_t> pay;
        while (reader.Poll(type, pay))
        {
            switch (static_cast<net::Msg>(type))
            {
            case net::Msg::Join:
            {
                if (pay.empty()) { sock->Close(); goto done; }
                uint8_t nl = pay[0];
                name.assign(reinterpret_cast<char*>(pay.data() + 1), nl);
                sessionId = g_nextSession++;

                PlayerView saved; bool has = LoadSaved(name, saved);

                // РћС‚РІРµС‚ JOIN_OK: session_id + has_saved + [x,y,z,heading]
                std::vector<uint8_t> resp;
                uint8_t le[8]; for (int i=0;i<8;++i) le[i]=(uint8_t)(sessionId>>(8*i));
                resp.insert(resp.end(), le, le+8);
                resp.push_back(has ? 1 : 0);
                if (has)
                {
                    auto pushf=[&](float v){ uint32_t u; std::memcpy(&u,&v,4); for(int i=0;i<4;++i) resp.push_back((uint8_t)(u>>(8*i))); };
                    pushf(saved.x); pushf(saved.y); pushf(saved.z); pushf(saved.heading);
                }
                std::vector<uint8_t> m = MakeMsg((uint8_t)net::Msg::JoinOk, resp.data(), (uint32_t)resp.size());
                sock->Send(m.data(), (int)m.size());

                // Р РµРіРёСЃС‚СЂРёСЂСѓРµРј Рё РѕРїРѕРІРµС‰Р°РµРј РѕСЃС‚Р°Р»СЊРЅС‹С…
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_players[name] = saved;
                    g_sockets.insert(sock);
                }
                // PLAYER_JOINED РѕСЃС‚Р°Р»СЊРЅС‹Рј (РєСЂРѕРјРµ РѕС‚РїСЂР°РІРёС‚РµР»СЏ вЂ” РѕРЅ СЃР°Рј Р·РЅР°РµС‚)
                std::cout << "[+] " << name << " joined from " << peer << (has ? " (saved loc)" : " (new)") << "\n";
                break;
            }
            case net::Msg::Frame:
            {
                // payload = GhostFrame bytes
                if (pay.size() >= sizeof(net::FramePayload))
                    RelayFrame(name, pay.data(), (uint32_t)pay.size());
                break;
            }
            case net::Msg::Leave:
                std::cout << "[-] " << name << " left\n";
                sock->Close();
                goto done;
            default:
                break;
            }
        }
    }
done:
    // СѓР±СЂР°С‚СЊ Рё СЃРѕС…СЂР°РЅРёС‚СЊ last-position РµСЃР»Рё РёРјСЏ Р±С‹Р»Рѕ
    if (!name.empty())
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_sockets.erase(sock);
        g_players.erase(name);
    }
    SaveAll();
}

} // namespace

int main(int argc, char** argv)
{
    if (!netio::Init()) { std::cerr << "socket init failed\n"; return 1; }

    int port = argc > 1 ? std::atoi(argv[1]) : 7777;

    // listen socket
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
        listen(ls, 8) == SOCKET_ERROR) { std::cerr << "bind/listen failed\n"; closesocket(ls); return 1; }
#else
    if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) == -1 ||
        ::listen(ls, 8) == -1) { std::cerr << "bind/listen failed\n"; ::close(ls); return 1; }
#endif

    std::cout << "RDR2 CO-OP relay listening on port " << port << "\n";

    for (;;)
    {
#ifdef _WIN32
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
#else
        SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
#endif
        netio::Socket* cs = new netio::Socket(c);
        std::thread([cs]() {
            HandleClient(cs, "peer");
            delete cs;
        }).detach();
    }

    netio::Shutdown();
    return 0;
}
