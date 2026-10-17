#pragma once

// Сетевой клиент релей-сервера, используемый внутри ASI-плагина.
// Подключается к серверу, выполняет JOIN, периодически отправляет свой
// GhostFrame и принимает кадры других игроков (передавая их наружу через
// колбэки). Работает в фоновом потоке, чтобы не блокировать игровой tick.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "net/frame.h"
#include "net/protocol.h"
#include "net/socket.h"
#include "net/udp.h"

namespace netio {

// Состояние клиента.
enum class ClientState {
    Disconnected,
    Connecting,
    JoinOk,   // JOIN_OK получен; savedPos необязателен
    Error,
};

// Результат JOIN_OK от сервера (персистентность + роль).
struct JoinResult {
    bool hasSaved = false;
    float x = 0, y = 0, z = 0, heading = 0;
    bool isHost = false;   // сервер назначил хостом (первый подключившийся)
};

// Информация о подключившемся игроке (PLAYER_JOINED). Если hasPos=true,
// сервер прислал его последние/сохранённые координаты — точку спавна педа.
struct PlayerJoinInfo {
    std::string name;
    unsigned long long model = 0;
    bool hasPos = false;
    float x = 0, y = 0, z = 0, heading = 0;
};

class Client {
public:
    using OnFrame   = std::function<void(const net::FramePayload&)>;
    using OnJoinOk  = std::function<void(const JoinResult&)>;
    using OnPlayerJoin = std::function<void(const PlayerJoinInfo&)>;
    using OnPlayerLeave= std::function<void(const std::string&)>;
    using OnDied       = std::function<void(const std::string&)>; // игрок умер (имя)
    using OnRespawned  = std::function<void(const std::string&)>; // игрок ожил (имя)
    using OnWorldState = std::function<void(const net::WorldState&)>;
    using OnOutfit     = std::function<void(const net::OutfitState&)>;
    using OnShot       = std::function<void(const net::ShotPayload&)>; // выстрел

    Client();
    ~Client();

    void Start(const std::string& host, int port, const std::string& name,
               unsigned long long pedModel);
    void Stop();
    // Игровой поток вычисляет кадр (нативы) и «пушит» его сюда; отдельный
    // поток отправки шлёт последний снимок с фиксированными 60 Гц — джиттер
    // игрового цикла не сбивает сетевую частоту.
    void PushFrame(const net::FramePayload& f);
    void SendFrame(const net::FramePayload& f);
    // Сообщить серверу, что локальный игрок умер (лог в консоли сервера).
    void SendDied();
    // Сообщить серверу, что локальный игрок ожил после смерти.
    void SendRespawned();
    // Отправить хостом состояние мира (время+погода) всем остальным.
    void SendWorldState(const net::WorldState& ws);
    // Отправить компоненты одежды игрока (при смене наряда).
    void SendOutfit(const net::OutfitState& os);
    // Отправить событие выстрела (точка попадания/прицеливания).
    void SendShot(const net::ShotPayload& sp);
    // Последний измеренный RTT до сервера (мс), 0 = ещё не измерен.
    uint32_t RttMs() const { return rttMs_.load(); }

    void SetFrame(OnFrame cb);
    void SetJoinOk(OnJoinOk cb);
    void SetPlayerJoin(OnPlayerJoin cb);
    void SetPlayerLeave(OnPlayerLeave cb);
    void SetDied(OnDied cb);
    void SetRespawned(OnRespawned cb);
    void SetWorldState(OnWorldState cb);
    void SetOutfit(OnOutfit cb);
    void SetShot(OnShot cb);

    ClientState State() const;

private:
    void Run();
    bool SendRaw(const std::vector<uint8_t>& frame);
    void SendPing();
    uint64_t NowMs() const;
    // UDP-канал для кадров (Frame/Shot): отдельная дейтаграммная сессия.
    bool OpenUdp();
    void CloseUdp();
    void UdpSendFrame(const net::FramePayload& f);
    void UdpSendShot(const net::ShotPayload& sp);
    void UdpReadLoop();
    std::atomic<bool> udpRun_{false};
    std::thread udpThread_;
    SOCKET udpFd_{INVALID_SOCKET};
    bool udpReady_{false};
    int udpPort_{0};
    uint64_t sid_{0};
    sockaddr_in serverUdp_{};

    std::string host_;
    int         port_ = 0;
    std::string name_;
    unsigned long long pedModel_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<ClientState> state_{ClientState::Disconnected};
    std::thread th_;

    std::unique_ptr<Socket> sock_;
    netio::FrameReader reader_;

    OnFrame onFrame_;
    OnJoinOk onJoinOk_;
    OnPlayerJoin onPlayerJoin_;
    OnPlayerLeave onPlayerLeave_;
    OnDied onDied_;
    OnRespawned onRespawned_;
    OnWorldState onWorldState_;
    OnOutfit onOutfit_;
    OnShot onShot_;
    std::atomic<uint32_t> rttMs_{0};
    // Последний снимок кадра для потока отправки + сам поток.
    std::mutex frameMtx_;
    net::FramePayload latFrame_{};
    bool haveLatFrame_{false};
    std::atomic<bool> sendRun_{false};
    std::thread sendThread_;
};

} // namespace netio