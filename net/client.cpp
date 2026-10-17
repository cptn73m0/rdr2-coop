#include "client.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "frame.h"

namespace netio {

namespace {
// Локальный хелпер: собрать кадр Join (name + ped_model) для отправки.
std::vector<uint8_t> BuildJoinFrame(const std::string& name, unsigned long long pedModel)
{
    std::vector<uint8_t> pay;
    pay.push_back(static_cast<uint8_t>(name.size()));
    pay.insert(pay.end(), name.begin(), name.end());
    // ped_model: u64 little-endian
    for (int i = 0; i < 8; ++i)
        pay.push_back(static_cast<uint8_t>((pedModel >> (8 * i)) & 0xFF));
    return MakeFrame((uint8_t)net::Msg::Join, pay.data(), (uint32_t)pay.size());
}
} // namespace

Client::Client() = default;
Client::~Client() { Stop(); }

uint64_t Client::NowMs() const
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Client::SendPing()
{
    // Метка времени — в payload: сервер вернёт её в Pong, клиент посчитает RTT.
    uint64_t ts = NowMs();
    uint8_t pay[8];
    for (int i = 0; i < 8; ++i)
        pay[i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFF);
    SendRaw(MakeFrame((uint8_t)net::Msg::Ping, pay, sizeof(pay)));
}

void Client::SetFrame(OnFrame cb)          { onFrame_ = std::move(cb); }
void Client::SetJoinOk(OnJoinOk cb)        { onJoinOk_ = std::move(cb); }
void Client::SetPlayerJoin(OnPlayerJoin cb){ onPlayerJoin_ = std::move(cb); }
void Client::SetPlayerLeave(OnPlayerLeave cb) { onPlayerLeave_ = std::move(cb); }
void Client::SetDied(OnDied cb)                { onDied_ = std::move(cb); }
void Client::SetRespawned(OnRespawned cb)      { onRespawned_ = std::move(cb); }
void Client::SetWorldState(OnWorldState cb){ onWorldState_ = std::move(cb); }
void Client::SetOutfit(OnOutfit cb)        { onOutfit_ = std::move(cb); }
void Client::SetShot(OnShot cb)            { onShot_ = std::move(cb); }

ClientState Client::State() const { return state_.load(); }

void Client::Start(const std::string& host, int port, const std::string& name,
                   unsigned long long pedModel)
{
    Stop();
    host_ = host;
    port_ = port;
    name_ = name;
    pedModel_ = pedModel;
    running_.store(true);
    state_.store(ClientState::Connecting);
    th_ = std::thread([this] { Run(); });
}

void Client::Stop()
{
    running_.store(false);
    if (sock_) sock_->Close();
    if (th_.joinable()) th_.join();
    state_.store(ClientState::Disconnected);
}

bool Client::SendRaw(const std::vector<uint8_t>& frame)
{
    if (!sock_) return false;
    return sock_->Send(frame.data(), static_cast<int>(frame.size())) == Result::Ok;
}

void Client::SendFrame(const net::FramePayload& f)
{
    std::vector<uint8_t> pay(reinterpret_cast<const uint8_t*>(&f),
                             reinterpret_cast<const uint8_t*>(&f) + sizeof(f));
    std::vector<uint8_t> frame = MakeFrame((uint8_t)net::Msg::Frame, pay.data(), (uint32_t)pay.size());
    SendRaw(frame);
}

void Client::SendDied()
{
    // Сообщение без payload: сервер выводит лог о смерти игрока.
    SendRaw(MakeFrame((uint8_t)net::Msg::Died, nullptr, 0));
}

void Client::SendRespawned()
{
    // Сообщение без payload: сервер ретранслирует другим «игрок ожил».
    SendRaw(MakeFrame((uint8_t)net::Msg::Respawned, nullptr, 0));
}

void Client::SendWorldState(const net::WorldState& ws)
{
    std::vector<uint8_t> pay(reinterpret_cast<const uint8_t*>(&ws),
                             reinterpret_cast<const uint8_t*>(&ws) + sizeof(ws));
    std::vector<uint8_t> frame = MakeFrame((uint8_t)net::Msg::WorldState, pay.data(),
                                           (uint32_t)pay.size());
    SendRaw(frame);
}

void Client::SendOutfit(const net::OutfitState& os)
{
    std::vector<uint8_t> pay(reinterpret_cast<const uint8_t*>(&os),
                             reinterpret_cast<const uint8_t*>(&os) + sizeof(os));
    std::vector<uint8_t> frame = MakeFrame((uint8_t)net::Msg::Outfit, pay.data(),
                                           (uint32_t)pay.size());
    SendRaw(frame);
}

void Client::SendShot(const net::ShotPayload& sp)
{
    std::vector<uint8_t> pay(reinterpret_cast<const uint8_t*>(&sp),
                             reinterpret_cast<const uint8_t*>(&sp) + sizeof(sp));
    std::vector<uint8_t> frame = MakeFrame((uint8_t)net::Msg::Shot, pay.data(),
                                           (uint32_t)pay.size());
    SendRaw(frame);
}

void Client::Run()
{
    if (!netio::Init())
    {
        state_.store(ClientState::Error);
        running_.store(false);
        return;
    }

    SOCKET c = INVALID_SOCKET;
#ifdef _WIN32
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    c = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (c == INVALID_SOCKET) { state_.store(ClientState::Error); running_.store(false); return; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1)
    {
        closesocket(c);
        state_.store(ClientState::Error);
        running_.store(false);
        return;
    }

#ifdef _WIN32
    if (connect(c, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    { closesocket(c); state_.store(ClientState::Error); running_.store(false); return; }
#else
    if (::connect(c, (sockaddr*)&addr, sizeof(addr)) == -1)
    { ::close(c); state_.store(ClientState::Error); running_.store(false); return; }
#endif

    sock_ = std::make_unique<Socket>(c);
    // Отключаем алгоритм Нагла (иначе кадры копятся до ~40 мс) и переводим
    // сокет в неблокирующий режим: Send() только ставит данные в очередь.
    SetNoDelay(c);
    SetNonBlocking(c);
    state_.store(ClientState::JoinOk); // соединено; JOIN шлём дальше

    // Отправить JOIN.
    SendRaw(BuildJoinFrame(name_, pedModel_));

    uint8_t buf[4096];
    uint64_t lastPingMs = NowMs();
    while (running_.load())
    {
        sock_->Flush(); // дослать накопленный хвост (неблокирующий сокет)

        // Замер RTT: пинг раз в секунду, сервер отвечает Pong.
        uint64_t nowMs = NowMs();
        if (nowMs - lastPingMs >= 1000)
        {
            lastPingMs = nowMs;
            SendPing();
        }

        int n = 0;
        Result r = sock_->Recv(buf, sizeof(buf), n);
        if (r == Result::NoData || r == Result::Error) break;
        if (r == Result::Ok) reader_.Feed(buf, n);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1)); // не крутим CPU

        uint8_t type; std::vector<uint8_t> p;
        while (reader_.Poll(type, p))
        {
            switch (static_cast<net::Msg>(type))
            {
            case net::Msg::JoinOk:
            {
                JoinResult jr;
                if (p.size() < 10) break; // session(8) + has_saved(1) + is_host(1)
                // session_id (8) + has_saved (1) [+ coords 16] + is_host (1)
                uint64_t sid = 0;
                for (int i = 0; i < 8; ++i) sid |= (uint64_t)p[i] << (8 * i);
                (void)sid;
                jr.hasSaved = p[8] != 0;
                if (jr.hasSaved && p.size() >= 26)
                {
                    auto rf = [&](int off) {
                        uint32_t u = 0;
                        std::memcpy(&u, &p[off], 4);
                        float f;
                        std::memcpy(&f, &u, 4);
                        return f;
                    };
                    jr.x = rf(9); jr.y = rf(13); jr.z = rf(17); jr.heading = rf(21);
                    jr.isHost = p[25] != 0;
                }
                else
                {
                    jr.isHost = p[9] != 0; // без координат is_host сразу после has_saved
                }
                state_.store(ClientState::JoinOk);
                if (onJoinOk_) onJoinOk_(jr);
                break;
            }
            case net::Msg::PlayerJoined:
            {
                if (!p.empty())
                {
                    PlayerJoinInfo ji;
                    uint8_t nl = p[0];
                    ji.name.assign(reinterpret_cast<char*>(p.data() + 1), nl);
                    size_t off = 1u + nl;
                    if (p.size() >= off + 8)
                    {
                        for (int i = 0; i < 8; ++i)
                            ji.model |= (unsigned long long)p[off + i] << (8 * i);
                    }
                    // необязательный блок позиции: has_pos + [x,y,z,heading]
                    off += 8;
                    if (p.size() >= off + 1)
                    {
                        ji.hasPos = p[off] != 0;
                        off += 1;
                        if (ji.hasPos && p.size() >= off + 16)
                        {
                            auto rf = [&](int o) {
                                uint32_t u = 0;
                                std::memcpy(&u, &p[o], 4);
                                float f;
                                std::memcpy(&f, &u, 4);
                                return f;
                            };
                            ji.x = rf((int)off); ji.y = rf((int)off + 4);
                            ji.z = rf((int)off + 8); ji.heading = rf((int)off + 12);
                        }
                    }
                    if (onPlayerJoin_) onPlayerJoin_(ji);
                }
                break;
            }
            case net::Msg::Frame:
            {
                if (p.size() >= sizeof(net::FramePayload))
                {
                    net::FramePayload fp;
                    std::memcpy(&fp, p.data(), sizeof(fp));
                    if (onFrame_) onFrame_(fp);
                }
                break;
            }
            case net::Msg::PlayerLeft:
            {
                if (!p.empty())
                {
                    std::string nm(reinterpret_cast<char*>(p.data() + 1), p[0]);
                    if (onPlayerLeave_) onPlayerLeave_(nm);
                }
                break;
            }
            case net::Msg::Died:
            {
                // От сервера: игрок умер. name в payload (как PlayerLeft).
                if (!p.empty())
                {
                    std::string nm(reinterpret_cast<char*>(p.data() + 1), p[0]);
                    if (onDied_) onDied_(nm);
                }
                break;
            }
            case net::Msg::Respawned:
            {
                // От сервера: игрок ожил после смерти (name в payload).
                if (!p.empty())
                {
                    std::string nm(reinterpret_cast<char*>(p.data() + 1), p[0]);
                    if (onRespawned_) onRespawned_(nm);
                }
                break;
            }
            case net::Msg::WorldState:
            {
                if (p.size() >= sizeof(net::WorldState))
                {
                    net::WorldState ws;
                    std::memcpy(&ws, p.data(), sizeof(ws));
                    if (onWorldState_) onWorldState_(ws);
                }
                break;
            }
            case net::Msg::Outfit:
            {
                if (p.size() >= sizeof(net::OutfitState))
                {
                    net::OutfitState os;
                    std::memcpy(&os, p.data(), sizeof(os));
                    if (onOutfit_) onOutfit_(os);
                }
                break;
            }
            case net::Msg::Shot:
            {
                if (p.size() >= sizeof(net::ShotPayload))
                {
                    net::ShotPayload sp;
                    std::memcpy(&sp, p.data(), sizeof(sp));
                    if (onShot_) onShot_(sp);
                }
                break;
            }
            case net::Msg::Pong:
            {
                if (p.size() >= 8)
                {
                    uint64_t ts = 0;
                    for (int i = 0; i < 8; ++i)
                        ts |= static_cast<uint64_t>(p[i]) << (8 * i);
                    uint64_t now = NowMs();
                    if (now >= ts)
                        rttMs_.store(static_cast<uint32_t>(now - ts));
                }
                break;
            }
            default:
                break;
            }
        }
    }

    if (onPlayerLeave_) onPlayerLeave_(name_);
    if (sock_) sock_->Close();
    sock_.reset();
    running_.store(false);
    state_.store(ClientState::Disconnected);
}

} // namespace netio