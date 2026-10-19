// Headless test: Client reconnects with backoff after a fake server drops it.
// Server accepts a JOIN, replies JOIN_OK, closes after ~300 ms. Client must
// reconnect at least once (JoinOk >= 2, ConnectionLost >= 1).
#include "net/client.h"
#include "net/frame.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define closesocket close
#endif

namespace {

std::atomic<int> g_joins{0};
std::atomic<int> g_lost{0};
std::atomic<bool> g_run{true};

SOCKET CreateListener(int& outPort)
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return INVALID_SOCKET;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        closesocket(ls);
        return INVALID_SOCKET;
    }
    if (listen(ls, 4) != 0)
    {
        closesocket(ls);
        return INVALID_SOCKET;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
    {
        closesocket(ls);
        return INVALID_SOCKET;
    }
    outPort = ntohs(bound.sin_port);
    return ls;
}

void FakeServer(SOCKET ls)
{
    while (g_run.load())
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(ls, &set);
        timeval tv{0, 200000}; // 200 ms
        const int ready = select(static_cast<int>(ls) + 1, &set, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;

        // Read JOIN (we don't need to parse it).
        char buf[2048];
        (void)recv(c, buf, sizeof(buf), 0);

        // JOIN_OK: session(8) + hasSaved(0) + isHost(0) = 10 bytes.
        std::vector<uint8_t> pay(10, 0);
        pay[8] = 0; // hasSaved
        pay[9] = 0; // isHost
        auto frame = netio::MakeFrame(0x02, pay.data(), static_cast<uint32_t>(pay.size()));
        (void)send(c, reinterpret_cast<const char*>(frame.data()),
                   static_cast<int>(frame.size()), 0);

        // Stay connected briefly, then drop -> client should reconnect.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        closesocket(c);
    }
}

} // namespace

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::printf("WSAStartup failed\n");
        return 1;
    }
#endif
    if (!netio::Init())
    {
        std::printf("netio::Init failed\n");
        return 1;
    }

    int port = 0;
    SOCKET ls = CreateListener(port);
    if (ls == INVALID_SOCKET || port == 0)
    {
        std::printf("listener failed\n");
        return 1;
    }
    std::thread server(FakeServer, ls);

    netio::Client client;
    client.SetJoinOk([](const netio::JoinResult&) { ++g_joins; });
    client.SetConnectionLost([]() { ++g_lost; });
    client.Start("127.0.0.1", port, "ReconnectTest", 0xCF7E73BE);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (g_joins.load() >= 2 && g_lost.load() >= 1)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    client.Stop();
    g_run.store(false);
    server.join();
    closesocket(ls);
    netio::Shutdown();

    const int joins = g_joins.load();
    const int lost = g_lost.load();
    std::printf("client_reconnect_test: joins=%d lost=%d\n", joins, lost);
    if (joins < 2)
    {
        std::printf("FAIL: client did not reconnect\n");
        return 1;
    }
    if (lost < 1)
    {
        std::printf("FAIL: connection-lost callback not fired\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
