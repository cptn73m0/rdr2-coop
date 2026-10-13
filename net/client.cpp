#include "client.h"

#include <chrono>
#include <cstring>

#include "frame.h"

namespace netio {

namespace {
// Локальный хелпер: собрать кадр Join (name) для отправки.
std::vector<uint8_t> BuildJoinFrame(const std::string& name)
{
    std::vector<uint8_t> pay;
    pay.push_back(static_cast<uint8_t>(name.size()));
    pay.insert(pay.end(), name.begin(), name.end());
    return MakeFrame((uint8_t)net::Msg::Join, pay.data(), (uint32_t)pay.size());
}
} // namespace

Client::Client() = default;
Client::~Client() { Stop(); }

void Client::SetFrame(OnFrame cb)          { onFrame_ = std::move(cb); }
void Client::SetJoinOk(OnJoinOk cb)        { onJoinOk_ = std::move(cb); }
void Client::SetPlayerJoin(OnPlayerJoin cb){ onPlayerJoin_ = std::move(cb); }
void Client::SetPlayerLeave(OnPlayerLeave cb) { onPlayerLeave_ = std::move(cb); }

ClientState Client::State() const { return state_.load(); }

void Client::Start(const std::string& host, int port, const std::string& name)
{
    Stop();
    host_ = host;
    port_ = port;
    name_ = name;
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
    state_.store(ClientState::JoinOk); // соединено; JOIN шлём дальше

    // Отправить JOIN.
    SendRaw(BuildJoinFrame(name_));

    uint8_t buf[4096];
    while (running_.load())
    {
        int n = 0;
        Result r = sock_->Recv(buf, sizeof(buf), n);
        if (r == Result::NoData || r == Result::Error) break;
        if (r == Result::Ok) reader_.Feed(buf, n);

        uint8_t type; std::vector<uint8_t> p;
        while (reader_.Poll(type, p))
        {
            switch (static_cast<net::Msg>(type))
            {
            case net::Msg::JoinOk:
            {
                JoinResult jr;
                if (p.size() < 9) break;
                // session_id (8) + has_saved (1) [+ coords 16]
                uint64_t sid = 0;
                for (int i = 0; i < 8; ++i) sid |= (uint64_t)p[i] << (8 * i);
                (void)sid;
                jr.hasSaved = p[8] != 0;
                if (jr.hasSaved && p.size() >= 25)
                {
                    auto rf = [&](int off) {
                        uint32_t u = 0;
                        std::memcpy(&u, &p[off], 4);
                        float f;
                        std::memcpy(&f, &u, 4);
                        return f;
                    };
                    jr.x = rf(9); jr.y = rf(13); jr.z = rf(17); jr.heading = rf(21);
                }
                state_.store(ClientState::JoinOk);
                if (onJoinOk_) onJoinOk_(jr);
                break;
            }
            case net::Msg::PlayerJoined:
            {
                if (!p.empty())
                {
                    std::string nm(reinterpret_cast<char*>(p.data() + 1), p[0]);
                    if (onPlayerJoin_) onPlayerJoin_(nm);
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