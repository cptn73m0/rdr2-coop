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

    Client();
    ~Client();

    void Start(const std::string& host, int port, const std::string& name,
               unsigned long long pedModel);
    void Stop();
    void SendFrame(const net::FramePayload& f);
    // Сообщить серверу, что локальный игрок умер (лог в консоли сервера).
    void SendDied();
    // Сообщить серверу, что локальный игрок ожил после смерти.
    void SendRespawned();
    // Отправить хостом состояние мира (время+погода) всем остальным.
    void SendWorldState(const net::WorldState& ws);
    // Отправить компоненты одежды игрока (при смене наряда).
    void SendOutfit(const net::OutfitState& os);

    void SetFrame(OnFrame cb);
    void SetJoinOk(OnJoinOk cb);
    void SetPlayerJoin(OnPlayerJoin cb);
    void SetPlayerLeave(OnPlayerLeave cb);
    void SetDied(OnDied cb);
    void SetRespawned(OnRespawned cb);
    void SetWorldState(OnWorldState cb);
    void SetOutfit(OnOutfit cb);

    ClientState State() const;

private:
    void Run();
    bool SendRaw(const std::vector<uint8_t>& frame);

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
};

} // namespace netio