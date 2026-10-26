#pragma once

// Сетевой клиент релей-сервера, используемый внутри ASI-плагина.
// Подключается к серверу, выполняет JOIN, периодически отправляет свой
// GhostFrame и принимает кадры других игроков (передавая их наружу через
// колбэки). Работает в фоновом потоке, чтобы не блокировать игровой tick.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    // R&D: TrainSnapshot (сырой payload, разбор — в coop).
    using OnTrainSnapshot = std::function<void(const std::vector<uint8_t>&)>;
    // P3: активная миссия хоста.
    using OnMissionState = std::function<void(const net::MissionStatePayload&)>;
    // P3: барьер совместного старта миссии.
    using OnMissionBarrier = std::function<void(const net::MissionBarrierPayload&)>;
    // Соединение потеряно (не ручной Stop) — клиент уйдёт в reconnect.
    using OnConnectionLost = std::function<void()>;
    // Сервер сменил роль (host migration).
    using OnHostChanged = std::function<void(bool isHost)>;
    // E0 (v15): actor-репликация. Типизированные колбэки для приёма:
    // 0x18-0x1C приходят только от хоста (через сервер), 0x1D — хосту от гостя.
    using OnActorSpawnBatch = std::function<void(
        uint32_t epoch, const std::vector<net::ActorSpawn>& actors)>;
    using OnActorFrames = std::function<void(
        uint32_t epoch, uint16_t seq, const std::vector<net::ActorFrame>& frames)>;
    using OnActorDespawn = std::function<void(const net::ActorDespawnPayload&)>;
    using OnActorEvent = std::function<void(const net::ActorEventPayload&)>;
    using OnActorOutfit = std::function<void(
        uint32_t epoch, uint32_t actorId, const net::OutfitState& outfit)>;
    using OnActorResyncRequest = std::function<void(const net::ActorResyncPayload&)>;
    // E2c-b (v16): точная внешность NPC (asset/tint) от хоста.
    using OnActorAppearance = std::function<void(
        uint32_t epoch, uint32_t actorId,
        const std::vector<net::ActorAppearanceEntry>& entries)>;
    // E5 (v17): хост получает запрос урона по NPC от гостя (0x1F).
    using OnActorDamageRequest = std::function<void(
        uint32_t epoch, uint32_t actorId, uint16_t damage, uint32_t weaponHash)>;
    // E6 (v18): UI-статус другого игрока (карта/пауза/сатчель/магазин).
    using OnPlayerPresence = std::function<void(uint32_t epoch, uint16_t flags)>;
    // E3d-c (v19): occupants транспорта от хоста (гость).
    using OnVehicleOccupants = std::function<void(
        uint32_t vehicleNetId,
        const std::vector<net::VehicleOccupantEntry>& entries)>;
    // E3d-b (v19): снимок поезда/состава v2 от хоста (гость).
    using OnTrainSnapshotV2 = std::function<void(
        const net::TrainSnapshotV2Payload& payload)>;

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
    // Отправить снаряжение лошади (shop-предметы: седло/поклада) при смене.
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
    // R&D: отправить TrainSnapshot (только хост).
    void SendTrainSnapshot(const std::vector<uint8_t>& payload);
    // E0 (v15): отправка actor-сообщений (хост). ActorResync — гостевая
    // заявка, её шлёт игровой слой через SendActorResync ниже.
    void SendActorSpawnBatch(uint32_t epoch, const std::vector<net::ActorSpawn>& actors);
    void SendActorFrames(uint32_t epoch, uint16_t seq,
                         const std::vector<net::ActorFrame>& frames);
    void SendActorDespawn(const net::ActorDespawnPayload& msg);
    void SendActorEvent(const net::ActorEventPayload& ev);
    void SendActorOutfit(uint32_t epoch, uint32_t actorId, const net::OutfitState& outfit);
    // E2c-b (v16): отправить внешность актёра (хост). Пустой список не шлётся
    // (count=0 невалиден на проводе); count = число валидных компонентов,
    // максимум kActorAppearanceMaxComponents=16; reserved обнуляется.
    void SendActorAppearance(uint32_t epoch, uint32_t actorId,
                             const std::vector<net::ActorAppearanceEntry>& entries);
    // E1 (v15): resync-запрос гостя хосту (0x1D). Сервер валидирует и релеит
    // ТОЛЬКО хосту. Возвращает false, если ещё нет JOIN_OK / сокет закрыт /
    // кадр не удалось поставить в очередь.
    bool SendActorResync(uint32_t epoch, uint32_t lastSeq, uint8_t reason);
    // E5 (v17): гость сообщает хосту урон по реплицированному NPC (0x1F).
    // Сервер релеит ТОЛЬКО хосту; хост не шлёт. damage клампится в
    // 1..kMaxActorDamage; при epoch/actorId=0 или роли хоста не отправляется.
    // Возвращает false, если ещё нет JOIN_OK / сокет закрыт / не гость.
    bool SendActorDamage(uint32_t epoch, uint32_t actorId, uint16_t damage,
                         uint32_t weaponHash);
    // E6 (v18): сообщить свой UI-статус (карта/пауза/сатчель/магазин...) —
    // любой роль; сервер релеит ДРУГОМУ игроку. flags маскируются известными
    // битами; 0 = нет статуса. Возвращает false до JOIN_OK/при epoch=0.
    bool SendPlayerPresence(uint32_t epoch, uint16_t flags);
    // E3d-c (v19): отправить occupants транспорта (хост). count 0 — очистить
    // occupants; entries клампится до kMaxVehicleOccupants.
    void SendVehicleOccupants(
        uint32_t vehicleNetId,
        const std::vector<net::VehicleOccupantEntry>& entries);
    // E3d-b (v19): отправить снимок поезда/состава v2 (хост).
    void SendTrainSnapshotV2(const net::TrainSnapshotV2Payload& payload);
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
    void SetTrainSnapshot(OnTrainSnapshot cb);
    void SetConnectionLost(OnConnectionLost cb);
    void SetHostChanged(OnHostChanged cb);
    // E0 (v15): actor-сообщения.
    void SetActorSpawnBatch(OnActorSpawnBatch cb);
    void SetActorFrames(OnActorFrames cb);
    void SetActorDespawn(OnActorDespawn cb);
    void SetActorEvent(OnActorEvent cb);
    void SetActorOutfit(OnActorOutfit cb);
    void SetActorAppearance(OnActorAppearance cb);
    void SetActorResyncRequest(OnActorResyncRequest cb);
    void SetActorDamageRequest(OnActorDamageRequest cb);
    void SetPlayerPresence(OnPlayerPresence cb);
    void SetVehicleOccupants(OnVehicleOccupants cb);
    void SetTrainSnapshotV2(OnTrainSnapshotV2 cb);

    ClientState State() const;

private:
    void Run();
    bool RunOnce();
    // Текущий сокет (копия shared_ptr под mutex): объект живёт, пока мы его
    // держим, даже если сетевой поток уже заменил/закрыл соединение.
    std::shared_ptr<Socket> CurrentSocket();
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

    // sock_ читается игровым потоком (SendRaw/Stop) и пересоздаётся сетевым
    // потоком — доступ только под sockMtx_.
    std::mutex sockMtx_;
    std::shared_ptr<Socket> sock_;

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
    OnTrainSnapshot onTrainSnapshot_;
    OnConnectionLost onConnectionLost_;
    OnHostChanged onHostChanged_;
    OnActorSpawnBatch onActorSpawnBatch_;
    OnActorFrames onActorFrames_;
    OnActorDespawn onActorDespawn_;
    OnActorEvent onActorEvent_;
    OnActorOutfit onActorOutfit_;
    OnActorAppearance onActorAppearance_;
    OnActorResyncRequest onActorResyncRequest_;
    OnActorDamageRequest onActorDamageRequest_;
    OnPlayerPresence onPlayerPresence_;
    OnVehicleOccupants onVehicleOccupants_;
    OnTrainSnapshotV2 onTrainSnapshotV2_;
    std::atomic<bool> joined_{false};
    std::atomic<bool> isHost_{false};
    std::atomic<uint32_t> rttMs_{0};
};

} // namespace netio
