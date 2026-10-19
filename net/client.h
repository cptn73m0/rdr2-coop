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
    using OnShot       = std::function<void(const net::ShotPayload&)>; // выстрел
    using OnHorseOutfit= std::function<void(const net::OutfitState&)>; // снаряжение лошади
    using OnJoinRejected = std::function<void(net::JoinRejectReason)>;
    // P1: сырой payload WorldSnapshot (разбор — в игровом потоке, в coop).
    using OnWorldSnapshot = std::function<void(const std::vector<uint8_t>&)>;
    // P2: ActionState от удалённого игрока (копия в Tick).
    using OnActionState = std::function<void(const net::ActionStatePayload&)>;
    // P2: посадка удалённого игрока в транспорт.
    using OnPlayerVehicle = std::function<void(const net::PlayerVehicleState&)>;
    // P3: событие MissionData (хост завершил миссию).
    using OnMissionEvent = std::function<void(const net::MissionEventPayload&)>;
    // P3: активная миссия хоста.
    using OnMissionState = std::function<void(const net::MissionStatePayload&)>;
    // P3: барьер совместного старта миссии.
    using OnMissionBarrier = std::function<void(const net::MissionBarrierPayload&)>;
    // Соединение потеряно (не ручной Stop) — клиент уйдёт в reconnect.
    using OnConnectionLost = std::function<void()>;
    // Сервер сменил роль (host migration).
    using OnHostChanged = std::function<void(bool isHost)>;

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
    // Отправить событие выстрела (точка попадания/прицеливания).
    void SendShot(const net::ShotPayload& sp);
    // Отправить снаряжение лошади (shop-предметы: седло/поклажа) при смене.
    void SendHorseOutfit(const net::OutfitState& os);
    // P1: отправить снапшот мира (только хост).
    void SendWorldSnapshot(const std::vector<uint8_t>& payload);
    // P2: отправить состояние действия/сценария своего персонажа.
    void SendActionState(const net::ActionStatePayload& state);
    // P2: отправить состояние посадки в транспорт своего персонажа.
    void SendPlayerVehicle(const net::PlayerVehicleState& state);
    // P3: отправить событие завершённой миссии (только хост).
    void SendMissionEvent(const net::MissionEventPayload& event);
    // P3: отправить активную миссию (только хост).
    void SendMissionState(const net::MissionStatePayload& state);
    // P3: отправить событие барьера миссии.
    void SendMissionBarrier(const net::MissionBarrierPayload& event);
    // Текущее состояние роли по мнению сервера (обновляется HostChanged).
    bool IsHost() const { return isHost_.load(); }
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
    void SetHorseOutfit(OnHorseOutfit cb);
    void SetJoinRejected(OnJoinRejected cb);
    void SetWorldSnapshot(OnWorldSnapshot cb);
    void SetActionState(OnActionState cb);
    void SetPlayerVehicle(OnPlayerVehicle cb);
    void SetMissionEvent(OnMissionEvent cb);
    void SetMissionState(OnMissionState cb);
    void SetMissionBarrier(OnMissionBarrier cb);
    void SetConnectionLost(OnConnectionLost cb);
    void SetHostChanged(OnHostChanged cb);

    ClientState State() const;

private:
    void Run();
    bool RunOnce();
    bool SendRaw(const std::vector<uint8_t>& frame);
    void SendPing();
    uint64_t NowMs() const;

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
    OnHorseOutfit onHorseOutfit_;
    OnJoinRejected onJoinRejected_;
    OnWorldSnapshot onWorldSnapshot_;
    OnActionState onActionState_;
    OnPlayerVehicle onPlayerVehicle_;
    OnMissionEvent onMissionEvent_;
    OnMissionState onMissionState_;
    OnMissionBarrier onMissionBarrier_;
    OnConnectionLost onConnectionLost_;
    OnHostChanged onHostChanged_;
    std::atomic<bool> joined_{false};
    std::atomic<bool> isHost_{false};
    std::atomic<uint32_t> rttMs_{0};
};

} // namespace netio