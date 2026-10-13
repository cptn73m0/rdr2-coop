#pragma once

// Сетевой клиент релей-сервера, используемый внутри ASI-плагина.
// Подключается к серверу, выполняет JOIN, периодически отправляет свой
// GhostFrame и принимает кадры других игроков (передавая их наружу через
// колбэки). Работает в фоновом потоке, чтобы не блокировать игровой tick.

#include <atomic>
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

// Результат JOIN_OK от сервера (персистентность).
struct JoinResult {
    bool hasSaved = false;
    float x = 0, y = 0, z = 0, heading = 0;
};

class Client {
public:
    // Колбэки из игрового слоя. Вызываются из фонового потока клиента.
    using OnFrame   = std::function<void(const net::FramePayload&)>;
    using OnJoinOk  = std::function<void(const JoinResult&)>;
    using OnPlayerJoin = std::function<void(const std::string& name)>;
    using OnPlayerLeave= std::function<void(const std::string& name)>;

    Client();
    ~Client();

    // Запустить подключение к host:port с именем name.
    void Start(const std::string& host, int port, const std::string& name);

    // Остановить (разорвать соединение).
    void Stop();

    // Отправить свой кадр состояния (вызывается из игрового tick).
    void SendFrame(const net::FramePayload& f);

    // Регистрация колбэков.
    void SetFrame(OnFrame cb)        { onFrame_ = std::move(cb); }
    void SetJoinOk(OnJoinOk cb)      { onJoinOk_ = std::move(cb); }
    void SetPlayerJoin(OnPlayerJoin cb) { onPlayerJoin_ = std::move(cb); }
    void SetPlayerLeave(OnPlayerLeave cb) { onPlayerLeave_ = std::move(cb); }

    ClientState State() const { return state_.load(); }

private:
    void Run();
    void ProcessIncoming();
    bool SendRaw(const std::vector<uint8_t>& frame);

    std::string host_;
    int         port_ = 0;
    std::string name_;

    std::atomic<bool> running_{false};
    std::atomic<ClientState> state_{ClientState::Disconnected};
    std::thread th_;

    std::unique_ptr<Socket> sock_;
    FrameReader reader_;

    OnFrame onFrame_;
    OnJoinOk onJoinOk_;
    OnPlayerJoin onPlayerJoin_;
    OnPlayerLeave onPlayerLeave_;
};

} // namespace netio