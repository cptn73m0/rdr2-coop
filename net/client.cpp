#include "client.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#ifndef _WIN32
#  include <sys/select.h>
#endif

#include "frame.h"

namespace netio {

namespace {

// P0: таймауты. Без них клиент мог зависнуть в Connecting навсегда, если TCP
// поднят, а сервер не отвечает (или процесс сервера завис).
constexpr uint64_t kConnectTimeoutMs = 3000; // неблокирующий connect
constexpr uint64_t kJoinTimeoutMs    = 5000; // ожидание JOIN_OK после JOIN
constexpr uint64_t kPongTimeoutMs    = 5000; // нет Pong при joined_ → reconnect
constexpr uint64_t kPingIntervalMs   = 1000;

// Ник из ini не должен ломать протокол/лог/players.txt сервера:
// - убираем control-байты (\0, \r, \n и т.п.);
// - ':' заменяем на '_' (разделитель формата сохранений);
// - обрезаем до 32 байт по границе UTF-8 (серверный лимит);
// - пустой ник заменяем на "Player".
// Иначе сервер закрывал соединение молча и клиент бесконечно реконнектился.
constexpr std::size_t kMaxNameBytes = 32;

std::string SanitizePlayerName(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (const unsigned char c : in)
    {
        if (c < 0x20 || c == 0x7F) continue; // control-байты, включая \0 \r \n
        out.push_back(c == ':' ? '_' : static_cast<char>(c));
    }

    // Обрезаем пробелы по краям (ник "  Player  " → "Player").
    const auto first = out.find_first_not_of(" \t");
    if (first == std::string::npos)
        out.clear();
    else
    {
        const auto last = out.find_last_not_of(" \t");
        out = out.substr(first, last - first + 1);
    }

    if (out.size() > kMaxNameBytes)
    {
        std::size_t cut = kMaxNameBytes;
        // Не разрезаем UTF-8 последовательность посередине.
        while (cut > 0 && cut < out.size() &&
               (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    if (out.empty())
        out = "Player";
    return out;
}

int LastSocketError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool ConnectInProgress(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return e == EINPROGRESS || e == EALREADY || e == EWOULDBLOCK;
#endif
}

// Локальный хелпер: собрать кадр Join (name + ped_model + protocol_version).
std::vector<uint8_t> BuildJoinFrame(const std::string& name, unsigned long long pedModel)
{
    std::vector<uint8_t> pay;
    pay.push_back(static_cast<uint8_t>(name.size()));
    pay.insert(pay.end(), name.begin(), name.end());
    // ped_model: u64 little-endian
    for (int i = 0; i < 8; ++i)
        pay.push_back(static_cast<uint8_t>((pedModel >> (8 * i)) & 0xFF));
    // protocol_version: u16 little-endian (Spike 1). Старые серверы без версии
    // отклонят JOIN, чтобы не разбирать несовместимые сообщения.
    pay.push_back(static_cast<uint8_t>(net::kProtocolVersion & 0xFF));
    pay.push_back(static_cast<uint8_t>((net::kProtocolVersion >> 8) & 0xFF));
    return MakeFrame((uint8_t)net::Msg::Join, pay.data(), (uint32_t)pay.size());
}

bool SendOn(const std::shared_ptr<Socket>& sock, const std::vector<uint8_t>& frame)
{
    if (!sock) return false;
    return sock->Send(frame.data(), static_cast<int>(frame.size())) == Result::Ok;
}

// Разбор строки "name_len:u8 + name[]" из сообщений сервера. Раньше клиент
// читал p.data()+1 без проверки длины (heap OOB при битом/враждебном сервере).
bool ParseNamePayload(const std::vector<uint8_t>& p, std::string& out)
{
    if (p.empty()) return false;
    const std::size_t len = p[0];
    if (len == 0 || len > net::kMaxName) return false;
    if (p.size() < 1 + len) return false;
    out.assign(reinterpret_cast<const char*>(p.data() + 1), len);
    return true;
}

bool IsFiniteCoord(float v)
{
    return std::isfinite(v) && std::fabs(v) < 1.0e6f;
}

// FRAME от удалённого игрока: NaN/Inf/абсурдные координаты не пускаем в игру
// (SetEntityCoords/TASK_* с ними — риск нестабильности и «телепортов»).
bool IsSaneFrame(const net::FramePayload& f)
{
    return IsFiniteCoord(f.x) && IsFiniteCoord(f.y) && IsFiniteCoord(f.z) &&
           IsFiniteCoord(f.heading) && std::isfinite(f.health) &&
           IsFiniteCoord(f.aimX) && IsFiniteCoord(f.aimY) &&
           IsFiniteCoord(f.aimZ) && std::isfinite(f.aimDist);
}

bool IsSaneWorldState(const net::WorldState& ws)
{
    return ws.hour < 24 && ws.minute < 60 && ws.second < 60 &&
           std::isfinite(ws.weatherPct) &&
           ws.weatherPct >= -0.01f && ws.weatherPct <= 1.01f;
}

bool IsSaneVehicle(const net::PlayerVehicleState& pv)
{
    return IsFiniteCoord(pv.x) && IsFiniteCoord(pv.y) &&
           IsFiniteCoord(pv.z) && IsFiniteCoord(pv.heading);
}

bool IsSaneWorldSnapshot(const std::vector<uint8_t>& p)
{
    if (p.size() < sizeof(net::WorldSnapshotHeader)) return false;
    net::WorldSnapshotHeader h{};
    std::memcpy(&h, p.data(), sizeof(h));
    if (h.count > net::kMaxWorldEntities) return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::WorldEntity);
    if (p.size() != need) return false;
    const uint8_t* data = p.data() + sizeof(h);
    for (uint16_t i = 0; i < h.count; ++i)
    {
        net::WorldEntity e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e), sizeof(e));
        if (!IsFiniteCoord(e.x) || !IsFiniteCoord(e.y) || !IsFiniteCoord(e.z) ||
            !IsFiniteCoord(e.heading))
            return false;
    }
    return true;
}

// ---------------- E0 (v15): actor-сообщения ----------------
// Проверки как для FramePayload: точный размер, count<=kMaxActors,
// epoch!=0, actorId!=0, конечные координаты.
bool IsSaneActorSpawn(const net::ActorSpawn& a)
{
    return a.actorId != 0 && IsFiniteCoord(a.x) && IsFiniteCoord(a.y) &&
           IsFiniteCoord(a.z) && IsFiniteCoord(a.heading);
}

bool IsSaneActorFrame(const net::ActorFrame& f)
{
    return f.actorId != 0 && IsFiniteCoord(f.x) && IsFiniteCoord(f.y) &&
           IsFiniteCoord(f.z) && IsFiniteCoord(f.heading);
}

bool ParseActorSpawnBatch(const std::vector<uint8_t>& p, uint32_t& outEpoch,
                          std::vector<net::ActorSpawn>& out)
{
    if (p.size() < sizeof(net::ActorSpawnBatchHeader)) return false;
    net::ActorSpawnBatchHeader h{};
    std::memcpy(&h, p.data(), sizeof(h));
    if (h.epoch == 0 || h.count > net::kMaxActors) return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::ActorSpawn);
    if (p.size() != need) return false;
    const uint8_t* data = p.data() + sizeof(h);
    out.clear();
    out.reserve(h.count);
    for (uint16_t i = 0; i < h.count; ++i)
    {
        net::ActorSpawn a{};
        std::memcpy(&a, data + static_cast<std::size_t>(i) * sizeof(a), sizeof(a));
        if (!IsSaneActorSpawn(a)) return false;
        out.push_back(a);
    }
    outEpoch = h.epoch;
    return true;
}

bool ParseActorFrames(const std::vector<uint8_t>& p, uint32_t& outEpoch,
                      uint16_t& outSeq, std::vector<net::ActorFrame>& out)
{
    if (p.size() < sizeof(net::ActorFramesHeader)) return false;
    net::ActorFramesHeader h{};
    std::memcpy(&h, p.data(), sizeof(h));
    if (h.epoch == 0 || h.count > net::kMaxActors) return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::ActorFrame);
    if (p.size() != need) return false;
    const uint8_t* data = p.data() + sizeof(h);
    out.clear();
    out.reserve(h.count);
    for (uint16_t i = 0; i < h.count; ++i)
    {
        net::ActorFrame f{};
        std::memcpy(&f, data + static_cast<std::size_t>(i) * sizeof(f), sizeof(f));
        if (!IsSaneActorFrame(f)) return false;
        out.push_back(f);
    }
    outEpoch = h.epoch;
    outSeq = h.seq;
    return true;
}

bool ParseActorDespawn(const std::vector<uint8_t>& p, net::ActorDespawnPayload& out)
{
    if (p.size() != sizeof(net::ActorDespawnPayload)) return false;
    std::memcpy(&out, p.data(), sizeof(out));
    return out.epoch != 0 && out.actorId != 0;
}

bool ParseActorEvent(const std::vector<uint8_t>& p, net::ActorEventPayload& out)
{
    if (p.size() != sizeof(net::ActorEventPayload)) return false;
    std::memcpy(&out, p.data(), sizeof(out));
    return out.epoch != 0 && out.actorId != 0;
}

bool ParseActorOutfit(const std::vector<uint8_t>& p, uint32_t& outEpoch,
                      uint32_t& outActorId, net::OutfitState& out)
{
    if (p.size() != sizeof(net::ActorOutfitPayload)) return false;
    net::ActorOutfitPayload msg{};
    std::memcpy(&msg, p.data(), sizeof(msg));
    if (msg.epoch == 0 || msg.actorId == 0) return false;
    outEpoch = msg.epoch;
    outActorId = msg.actorId;
    out = msg.outfit;
    if (out.count > static_cast<uint32_t>(net::kMaxOutfitComponents))
        out.count = static_cast<uint32_t>(net::kMaxOutfitComponents);
    return true;
}

bool ParseActorResync(const std::vector<uint8_t>& p, net::ActorResyncPayload& out)
{
    if (p.size() != sizeof(net::ActorResyncPayload)) return false;
    std::memcpy(&out, p.data(), sizeof(out));
    return out.epoch != 0;
}

// E2c-b (v16): точная внешность NPC. Точный размер, count
// 1..kActorAppearanceMaxComponents (16), epoch/actorId != 0, reserved == 0.
bool ParseActorAppearance(const std::vector<uint8_t>& p, uint32_t& outEpoch,
                          uint32_t& outActorId,
                          std::vector<net::ActorAppearanceEntry>& out)
{
    if (p.size() < sizeof(net::ActorAppearanceHeader)) return false;
    net::ActorAppearanceHeader h{};
    std::memcpy(&h, p.data(), sizeof(h));
    if (h.epoch == 0 || h.actorId == 0) return false;
    if (h.count < 1 ||
        h.count > static_cast<uint8_t>(net::kActorAppearanceMaxComponents))
        return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::ActorAppearanceEntry);
    if (p.size() != need) return false;
    const uint8_t* data = p.data() + sizeof(h);
    out.clear();
    out.reserve(h.count);
    for (uint8_t i = 0; i < h.count; ++i)
    {
        net::ActorAppearanceEntry e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e),
                    sizeof(e));
        if (e.reserved != 0) return false;
        out.push_back(e);
    }
    outEpoch = h.epoch;
    outActorId = h.actorId;
    return true;
}

// E5 (v17): запрос урона от гостя (0x1F). Точный размер 16, epoch/actorId != 0,
// damage 1..kMaxActorDamage.
bool ParseActorDamage(const std::vector<uint8_t>& p,
                      net::ActorDamagePayload& out)
{
    if (p.size() != sizeof(net::ActorDamagePayload)) return false;
    std::memcpy(&out, p.data(), sizeof(out));
    return out.epoch != 0 && out.actorId != 0 &&
           out.damage >= net::kMinActorDamage &&
           out.damage <= net::kMaxActorDamage;
}

// E6 (v18): UI-статус другого игрока (0x20). Точный размер 8, epoch != 0,
// только известные биты flags (неизвестные -> drop).
bool ParsePlayerPresence(const std::vector<uint8_t>& p,
                         net::PlayerPresencePayload& out)
{
    if (p.size() != sizeof(net::PlayerPresencePayload)) return false;
    std::memcpy(&out, p.data(), sizeof(out));
    if (out.epoch == 0) return false;
    return (out.flags & ~net::kPlayerPresenceKnownFlags) == 0;
}

// E3d-c (v19): occupants транспорта. Точный размер 8+count*8, count 0..8,
// vehicleNetId != 0; записи: actorId != 0, seat -1..8, carIndex<=16,
// harness<=5, только известные флаги.
bool ParseVehicleOccupants(const std::vector<uint8_t>& p,
                           uint32_t& outVehicleNetId,
                           std::vector<net::VehicleOccupantEntry>& out)
{
    if (p.size() < sizeof(net::VehicleOccupantsHeader)) return false;
    net::VehicleOccupantsHeader h{};
    std::memcpy(&h, p.data(), sizeof(h));
    if (h.vehicleNetId == 0) return false;
    if (h.count > net::kMaxVehicleOccupants) return false;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.count) * sizeof(net::VehicleOccupantEntry);
    if (p.size() != need) return false;
    const uint8_t* data = p.data() + sizeof(h);
    out.clear();
    out.reserve(h.count);
    for (uint8_t i = 0; i < h.count; ++i)
    {
        net::VehicleOccupantEntry e{};
        std::memcpy(&e, data + static_cast<std::size_t>(i) * sizeof(e),
                    sizeof(e));
        if (e.actorId == 0) return false;
        if (e.seat < net::kMinVehicleSeat || e.seat > net::kMaxVehicleSeat)
            return false;
        if ((e.flags & ~net::kVehicleOccupantKnownFlags) != 0) return false;
        if (e.carIndex > net::kMaxVehicleCarIndex) return false;
        if (e.harness > net::kMaxDraftHarness) return false;
        out.push_back(e);
    }
    outVehicleNetId = h.vehicleNetId;
    return true;
}

// E3d-b (v19): поезд/состав v2. Точный размер 48, epoch/trainId != 0,
// carCount 1..kMaxTrainCars, speed -40..40, координаты конечны, флаги известны.
bool ParseTrainSnapshotV2(const std::vector<uint8_t>& p,
                          net::TrainSnapshotV2Payload& out)
{
    if (p.size() != sizeof(net::TrainSnapshotV2Payload)) return false;
    std::memcpy(&out, p.data(), sizeof(out));
    if (out.epoch == 0 || out.trainId == 0) return false;
    if (out.carCount < 1 || out.carCount > net::kMaxTrainCars) return false;
    if (!std::isfinite(out.speed) || out.speed < net::kMinTrainSpeed ||
        out.speed > net::kMaxTrainSpeed)
        return false;
    if (!IsFiniteCoord(out.x) || !IsFiniteCoord(out.y) ||
        !IsFiniteCoord(out.z) || !IsFiniteCoord(out.heading))
        return false;
    return (out.flags & ~net::kTrainSnapshotV2KnownFlags) == 0;
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

std::shared_ptr<Socket> Client::CurrentSocket()
{
    std::lock_guard<std::mutex> lk(sockMtx_);
    return sock_;
}

bool Client::SendRaw(const std::vector<uint8_t>& frame)
{
    // До JOIN_OK игровой поток не должен слать ничего, кроме самого JOIN:
    // сервер закрывает соединение за сообщение до регистрации.
    if (!joined_.load()) return false;
    return SendOn(CurrentSocket(), frame);
}

void Client::SendPing()
{
    // Метка времени — в payload: сервер вернёт её в Pong, клиент посчитает RTT.
    if (!joined_.load()) return;
    auto sock = CurrentSocket();
    if (!sock) return;
    const uint64_t ts = NowMs();
    uint8_t pay[8];
    for (int i = 0; i < 8; ++i)
        pay[i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xFF);
    SendOn(sock, MakeFrame((uint8_t)net::Msg::Ping, pay, sizeof(pay)));
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
void Client::SetHorseOutfit(OnHorseOutfit cb) { onHorseOutfit_ = std::move(cb); }
void Client::SetJoinRejected(OnJoinRejected cb) { onJoinRejected_ = std::move(cb); }
void Client::SetWorldSnapshot(OnWorldSnapshot cb) { onWorldSnapshot_ = std::move(cb); }
void Client::SetActionState(OnActionState cb) { onActionState_ = std::move(cb); }
void Client::SetPlayerVehicle(OnPlayerVehicle cb) { onPlayerVehicle_ = std::move(cb); }
void Client::SetMissionEvent(OnMissionEvent cb) { onMissionEvent_ = std::move(cb); }
void Client::SetMissionState(OnMissionState cb) { onMissionState_ = std::move(cb); }
void Client::SetMissionBarrier(OnMissionBarrier cb) { onMissionBarrier_ = std::move(cb); }
void Client::SetTrainSnapshot(OnTrainSnapshot cb) { onTrainSnapshot_ = std::move(cb); }
void Client::SetConnectionLost(OnConnectionLost cb) { onConnectionLost_ = std::move(cb); }
void Client::SetHostChanged(OnHostChanged cb) { onHostChanged_ = std::move(cb); }
void Client::SetActorSpawnBatch(OnActorSpawnBatch cb) { onActorSpawnBatch_ = std::move(cb); }
void Client::SetActorFrames(OnActorFrames cb) { onActorFrames_ = std::move(cb); }
void Client::SetActorDespawn(OnActorDespawn cb) { onActorDespawn_ = std::move(cb); }
void Client::SetActorEvent(OnActorEvent cb) { onActorEvent_ = std::move(cb); }
void Client::SetActorOutfit(OnActorOutfit cb) { onActorOutfit_ = std::move(cb); }
void Client::SetActorAppearance(OnActorAppearance cb) { onActorAppearance_ = std::move(cb); }
void Client::SetActorResyncRequest(OnActorResyncRequest cb) { onActorResyncRequest_ = std::move(cb); }
void Client::SetActorDamageRequest(OnActorDamageRequest cb) { onActorDamageRequest_ = std::move(cb); }
void Client::SetPlayerPresence(OnPlayerPresence cb) { onPlayerPresence_ = std::move(cb); }
void Client::SetVehicleOccupants(OnVehicleOccupants cb) { onVehicleOccupants_ = std::move(cb); }
void Client::SetTrainSnapshotV2(OnTrainSnapshotV2 cb) { onTrainSnapshotV2_ = std::move(cb); }

ClientState Client::State() const { return state_.load(); }

void Client::Start(const std::string& host, int port, const std::string& name,
                   unsigned long long pedModel)
{
    Stop();
    host_ = host;
    port_ = port;
    name_ = SanitizePlayerName(name);
    pedModel_ = pedModel;
    joined_.store(false);
    isHost_.store(false);
    rttMs_.store(0);
    reader_.Reset();
    state_.store(ClientState::Connecting);
    running_.store(true);
    th_ = std::thread([this] { Run(); });
}

void Client::Stop()
{
    running_.store(false);
    // Close потокобезопасен и будит сетевой поток, если он в Recv/select.
    auto sock = CurrentSocket();
    if (sock) sock->Close();
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(sockMtx_);
        sock_.reset();
    }
    state_.store(ClientState::Disconnected);
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

void Client::SendHorseOutfit(const net::OutfitState& os)
{
    std::vector<uint8_t> pay(reinterpret_cast<const uint8_t*>(&os),
                             reinterpret_cast<const uint8_t*>(&os) + sizeof(os));
    std::vector<uint8_t> frame = MakeFrame((uint8_t)net::Msg::HorseOutfit, pay.data(),
                                            (uint32_t)pay.size());
    SendRaw(frame);
}

void Client::SendWorldSnapshot(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame = MakeFrame((uint8_t)net::Msg::WorldSnapshot,
                                           payload.data(), (uint32_t)payload.size());
    SendRaw(frame);
}

void Client::SendActionState(const net::ActionStatePayload& state)
{
    std::vector<uint8_t> frame = MakeFrame(
        (uint8_t)net::Msg::ActionState,
        reinterpret_cast<const uint8_t*>(&state), sizeof(state));
    SendRaw(frame);
}

void Client::SendPlayerVehicle(const net::PlayerVehicleState& state)
{
    std::vector<uint8_t> frame = MakeFrame(
        (uint8_t)net::Msg::PlayerVehicle,
        reinterpret_cast<const uint8_t*>(&state), sizeof(state));
    SendRaw(frame);
}

void Client::SendMissionEvent(const net::MissionEventPayload& event)
{
    std::vector<uint8_t> frame = MakeFrame(
        (uint8_t)net::Msg::MissionEvent,
        reinterpret_cast<const uint8_t*>(&event), sizeof(event));
    SendRaw(frame);
}

void Client::SendMissionState(const net::MissionStatePayload& state)
{
    std::vector<uint8_t> frame = MakeFrame(
        (uint8_t)net::Msg::MissionState,
        reinterpret_cast<const uint8_t*>(&state), sizeof(state));
    SendRaw(frame);
}

void Client::SendMissionBarrier(const net::MissionBarrierPayload& event)
{
    std::vector<uint8_t> frame = MakeFrame(
        (uint8_t)net::Msg::MissionBarrier,
        reinterpret_cast<const uint8_t*>(&event), sizeof(event));
    SendRaw(frame);
}

void Client::SendTrainSnapshot(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame = MakeFrame(
        (uint8_t)net::Msg::TrainSnapshot,
        payload.data(), (uint32_t)payload.size());
    SendRaw(frame);
}

void Client::SendActorSpawnBatch(uint32_t epoch, const std::vector<net::ActorSpawn>& actors)
{
    uint16_t count = static_cast<uint16_t>(actors.size());
    if (count > static_cast<uint16_t>(net::kMaxActors))
        count = static_cast<uint16_t>(net::kMaxActors);
    std::vector<uint8_t> pay(sizeof(net::ActorSpawnBatchHeader) +
                             static_cast<std::size_t>(count) * sizeof(net::ActorSpawn));
    net::ActorSpawnBatchHeader h{};
    h.epoch = epoch;
    h.count = count;
    std::memcpy(pay.data(), &h, sizeof(h));
    for (uint16_t i = 0; i < count; ++i)
    {
        std::memcpy(pay.data() + sizeof(h) +
                        static_cast<std::size_t>(i) * sizeof(net::ActorSpawn),
                    &actors[i], sizeof(net::ActorSpawn));
    }
    SendRaw(MakeFrame((uint8_t)net::Msg::ActorSpawnBatch,
                      pay.data(), static_cast<uint32_t>(pay.size())));
}

void Client::SendActorFrames(uint32_t epoch, uint16_t seq,
                             const std::vector<net::ActorFrame>& frames)
{
    uint16_t count = static_cast<uint16_t>(frames.size());
    if (count > static_cast<uint16_t>(net::kMaxActors))
        count = static_cast<uint16_t>(net::kMaxActors);
    std::vector<uint8_t> pay(sizeof(net::ActorFramesHeader) +
                             static_cast<std::size_t>(count) * sizeof(net::ActorFrame));
    net::ActorFramesHeader h{};
    h.epoch = epoch;
    h.seq = seq;
    h.count = count;
    std::memcpy(pay.data(), &h, sizeof(h));
    for (uint16_t i = 0; i < count; ++i)
    {
        std::memcpy(pay.data() + sizeof(h) +
                        static_cast<std::size_t>(i) * sizeof(net::ActorFrame),
                    &frames[i], sizeof(net::ActorFrame));
    }
    SendRaw(MakeFrame((uint8_t)net::Msg::ActorFrames,
                      pay.data(), static_cast<uint32_t>(pay.size())));
}

void Client::SendActorDespawn(const net::ActorDespawnPayload& msg)
{
    SendRaw(MakeFrame((uint8_t)net::Msg::ActorDespawn,
                      reinterpret_cast<const uint8_t*>(&msg),
                      static_cast<uint32_t>(sizeof(msg))));
}

void Client::SendActorEvent(const net::ActorEventPayload& ev)
{
    SendRaw(MakeFrame((uint8_t)net::Msg::ActorEvent,
                      reinterpret_cast<const uint8_t*>(&ev),
                      static_cast<uint32_t>(sizeof(ev))));
}

void Client::SendActorOutfit(uint32_t epoch, uint32_t actorId,
                             const net::OutfitState& outfit)
{
    net::ActorOutfitPayload msg{};
    msg.epoch = epoch;
    msg.actorId = actorId;
    msg.outfit = outfit;
    SendRaw(MakeFrame((uint8_t)net::Msg::ActorOutfit,
                      reinterpret_cast<const uint8_t*>(&msg),
                      static_cast<uint32_t>(sizeof(msg))));
}

bool Client::SendActorResync(uint32_t epoch, uint32_t lastSeq, uint8_t reason)
{
    // 0x1D — гость -> сервер -> хосту (сервер релеит только хосту). До JOIN_OK
    // не отправляем; SendRaw сам проверяет joined_/живость сокета и
    // потокобезопасно берёт текущее соединение (null/Closed -> false).
    if (!joined_.load()) return false;
    net::ActorResyncPayload msg{};
    msg.epoch = epoch;
    msg.lastSeq = lastSeq;
    msg.reason = reason;
    return SendRaw(MakeFrame((uint8_t)net::Msg::ActorResync,
                             reinterpret_cast<const uint8_t*>(&msg),
                             static_cast<uint32_t>(sizeof(msg))));
}

void Client::SendActorAppearance(
    uint32_t epoch, uint32_t actorId,
    const std::vector<net::ActorAppearanceEntry>& entries)
{
    // E2c-b (v16): 0x1E — хост -> сервер -> гостям. Пустой список/нулевые
    // epoch/actorId невалидны на проводе — не отправляем; reserved обнуляем.
    // count = число валидных компонентов (снимок), максимум 16 (460 B payload).
    if (epoch == 0 || actorId == 0 || entries.empty()) return;
    uint8_t count = static_cast<uint8_t>(entries.size());
    const uint8_t maxCount =
        static_cast<uint8_t>(net::kActorAppearanceMaxComponents);
    if (count > maxCount)
        count = maxCount;
    std::vector<uint8_t> pay(sizeof(net::ActorAppearanceHeader) +
        static_cast<std::size_t>(count) * sizeof(net::ActorAppearanceEntry));
    net::ActorAppearanceHeader h{};
    h.epoch = epoch;
    h.actorId = actorId;
    h.count = count;
    std::memcpy(pay.data(), &h, sizeof(h));
    for (uint8_t i = 0; i < count; ++i)
    {
        net::ActorAppearanceEntry e = entries[i];
        e.reserved = 0;
        std::memcpy(pay.data() + sizeof(h) +
                        static_cast<std::size_t>(i) * sizeof(e),
                    &e, sizeof(e));
    }
    if (pay.size() > net::kActorAppearanceMaxSize)
        return; // защита от рассинхрона констант
    SendRaw(MakeFrame((uint8_t)net::Msg::ActorAppearance,
                      pay.data(), static_cast<uint32_t>(pay.size())));
}

bool Client::SendActorDamage(uint32_t epoch, uint32_t actorId,
                             uint16_t damage, uint32_t weaponHash)
{
    // E5 (v17): 0x1F — гость -> сервер -> хосту. Хост урон применяет сам и
    // запросы не шлёт; до JOIN_OK тоже не шлём. damage клампится в 1..1000.
    if (!joined_.load()) return false;
    if (isHost_.load()) return false;
    if (epoch == 0 || actorId == 0) return false;
    if (damage < net::kMinActorDamage) damage = net::kMinActorDamage;
    if (damage > net::kMaxActorDamage) damage = net::kMaxActorDamage;
    net::ActorDamagePayload msg{};
    msg.epoch = epoch;
    msg.actorId = actorId;
    msg.damage = damage;
    msg.pad = 0;
    msg.weaponHash = weaponHash;
    return SendRaw(MakeFrame((uint8_t)net::Msg::ActorDamage,
                             reinterpret_cast<const uint8_t*>(&msg),
                             static_cast<uint32_t>(sizeof(msg))));
}

bool Client::SendPlayerPresence(uint32_t epoch, uint16_t flags)
{
    // E6 (v18): 0x20 — любой роль -> сервер -> другому игроку (без эха).
    // 0 = «нет специального статуса»; неизвестные биты маскируем, чтобы
    // не получать drop от сервера.
    if (!joined_.load()) return false;
    if (epoch == 0) return false;
    net::PlayerPresencePayload msg{};
    msg.epoch = epoch;
    msg.flags = static_cast<uint16_t>(flags & net::kPlayerPresenceKnownFlags);
    msg.pad = 0;
    return SendRaw(MakeFrame((uint8_t)net::Msg::PlayerPresence,
                             reinterpret_cast<const uint8_t*>(&msg),
                             static_cast<uint32_t>(sizeof(msg))));
}

void Client::SendVehicleOccupants(
    uint32_t vehicleNetId,
    const std::vector<net::VehicleOccupantEntry>& entries)
{
    // E3d-c (v19): 0x21 — хост -> сервер -> гостю. count 0 — очистить
    // occupants; count клампится до kMaxVehicleOccupants.
    if (vehicleNetId == 0) return;
    uint8_t count = static_cast<uint8_t>(entries.size());
    if (count > static_cast<uint8_t>(net::kMaxVehicleOccupants))
        count = static_cast<uint8_t>(net::kMaxVehicleOccupants);
    std::vector<uint8_t> pay(sizeof(net::VehicleOccupantsHeader) +
        static_cast<std::size_t>(count) * sizeof(net::VehicleOccupantEntry));
    net::VehicleOccupantsHeader h{};
    h.vehicleNetId = vehicleNetId;
    h.count = count;
    std::memcpy(pay.data(), &h, sizeof(h));
    for (uint8_t i = 0; i < count; ++i)
        std::memcpy(pay.data() + sizeof(h) +
                        static_cast<std::size_t>(i) * sizeof(net::VehicleOccupantEntry),
                    &entries[i], sizeof(net::VehicleOccupantEntry));
    SendRaw(MakeFrame((uint8_t)net::Msg::VehicleOccupants,
                      pay.data(), static_cast<uint32_t>(pay.size())));
}

void Client::SendTrainSnapshotV2(const net::TrainSnapshotV2Payload& payload)
{
    // E3d-b (v19): 0x22 — хост -> сервер -> гостю (48 B, байт-в-байт).
    SendRaw(MakeFrame((uint8_t)net::Msg::TrainSnapshotV2,
                      reinterpret_cast<const uint8_t*>(&payload),
                      static_cast<uint32_t>(sizeof(payload))));
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

    int backoffMs = 1000;
    while (running_.load())
    {
        const bool wasJoined = joined_.load();
        state_.store(ClientState::Connecting);
        joined_.store(false);
        reader_.Reset();
        {
            std::lock_guard<std::mutex> lk(sockMtx_);
            sock_.reset();
        }

        RunOnce(); // блокирующий цикл соединения/приёма

        if (!running_.load())
            break;

        // Потеря связи (не Stop): сообщаем наружу и уходим в reconnect.
        if (wasJoined && onConnectionLost_)
            onConnectionLost_();

        state_.store(ClientState::Disconnected);
        if (joined_.load())
            backoffMs = 1000; // успешный сеанс — начинаем backoff заново

        // Прерываемый сон: Stop не должен ждать весь backoff.
        int slept = 0;
        while (slept < backoffMs && running_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            slept += 100;
        }
        backoffMs = (backoffMs < 10000) ? backoffMs * 2 : 10000;
    }
    {
        std::lock_guard<std::mutex> lk(sockMtx_);
        sock_.reset();
    }
    if (state_.load() != ClientState::Error)
        state_.store(ClientState::Disconnected);
}

bool Client::RunOnce()
{
    SOCKET c = INVALID_SOCKET;
#ifdef _WIN32
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    c = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (c == INVALID_SOCKET) { state_.store(ClientState::Error); return false; }

    // Резолвим хост через getaddrinfo: поддерживаются и имена, и IPv4-литералы
    // (раньше inet_pton молча не принимал hostname и клиент ретраился вечно).
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port_);
    bool resolved = false;
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* ai = nullptr;
        if (getaddrinfo(host_.c_str(), nullptr, &hints, &ai) == 0 && ai)
        {
            for (addrinfo* it = ai; it; it = it->ai_next)
            {
                if (it->ai_family == AF_INET && it->ai_addr &&
                    it->ai_addrlen >= sizeof(sockaddr_in))
                {
                    std::memcpy(&addr.sin_addr,
                                &reinterpret_cast<const sockaddr_in*>(it->ai_addr)->sin_addr,
                                sizeof(addr.sin_addr));
                    resolved = true;
                    break;
                }
            }
            freeaddrinfo(ai);
        }
    }
    if (!resolved)
    {
        closesocket(c);
        state_.store(ClientState::Connecting);
        return false;
    }

    // Неблокирующий connect с таймаутом: Stop() не должен висеть до
    // системного таймаута (~20 с), если сервер недоступен.
    SetNonBlocking(c);
    bool connected = false;
#ifdef _WIN32
    int rc = connect(c, (sockaddr*)&addr, sizeof(addr));
#else
    int rc = ::connect(c, (sockaddr*)&addr, sizeof(addr));
#endif
    if (rc == 0)
    {
        connected = true;
    }
    else if (ConnectInProgress(LastSocketError()))
    {
        const uint64_t startMs = NowMs();
        while (running_.load() && NowMs() - startMs < kConnectTimeoutMs)
        {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(c, &wfds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000; // 100 мс — чтобы Stop прерывал быстро
#ifdef _WIN32
            const int sel = select(0, nullptr, &wfds, nullptr, &tv);
#else
            const int sel = select(static_cast<int>(c) + 1, nullptr, &wfds, nullptr, &tv);
#endif
            if (sel > 0)
            {
                int soErr = 0;
                socklen_t soLen = sizeof(soErr);
                if (getsockopt(c, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&soErr), &soLen) == 0 &&
                    soErr == 0)
                    connected = true;
                break;
            }
        }
    }

    if (!running_.load() || !connected)
    {
        closesocket(c);
        state_.store(ClientState::Connecting);
        return false;
    }

    auto sock = std::make_shared<Socket>(c);
    {
        std::lock_guard<std::mutex> lk(sockMtx_);
        sock_ = sock;
    }
    // Нагл отключаем сразу; неблокирующий режим уже включён.
    SetNoDelay(c);
    SetNonBlocking(c);
    state_.store(ClientState::Connecting); // соединено; JOIN шлём дальше

    // Отправить JOIN (напрямую, минуя gated SendRaw).
    SendOn(sock, BuildJoinFrame(name_, pedModel_));

    const uint64_t joinSentMs = NowMs();
    uint64_t lastPingMs = joinSentMs;
    uint64_t lastPongMs = joinSentMs;
    bool reconnectRequested = false;

    uint8_t buf[4096];
    while (running_.load())
    {
        sock->Flush(); // дослать накопленный хвост (неблокирующий сокет)

        const uint64_t nowMs = NowMs();
        if (joined_.load() && nowMs - lastPingMs >= kPingIntervalMs)
        {
            lastPingMs = nowMs;
            SendPing();
        }
        // Таймауты: без JoinOk или без Pong соединение считается мёртвым
        // (сервер завис/пропал, TCP при этом может быть «жив»).
        if (!joined_.load() && nowMs - joinSentMs > kJoinTimeoutMs)
        {
            reconnectRequested = true;
            break;
        }
        if (joined_.load() && nowMs - lastPongMs > kPongTimeoutMs)
        {
            reconnectRequested = true;
            break;
        }

        int n = 0;
        const Result r = sock->Recv(buf, sizeof(buf), n);
        // Closed — сокет закрыт нами/другим потоком (например, Send обнаружил
        // обрыв). Раньше этот результат крутил бы цикл без reconnect.
        if (r == Result::NoData || r == Result::Error || r == Result::Closed)
            break;
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
                if (jr.hasSaved)
                {
                    if (p.size() < 26) break; // битый JOIN_OK — пропускаем
                    auto rf = [&](int off) {
                        uint32_t u = 0;
                        std::memcpy(&u, &p[off], 4);
                        float f;
                        std::memcpy(&f, &u, 4);
                        return f;
                    };
                    jr.x = rf(9); jr.y = rf(13); jr.z = rf(17); jr.heading = rf(21);
                    jr.isHost = p[25] != 0;
                    if (!IsFiniteCoord(jr.x) || !IsFiniteCoord(jr.y) ||
                        !IsFiniteCoord(jr.z) || !IsFiniteCoord(jr.heading))
                    {
                        jr.hasSaved = false; // битые координаты не применяем
                        jr.x = jr.y = jr.z = jr.heading = 0.f;
                    }
                }
                else
                {
                    jr.isHost = p[9] != 0; // без координат is_host сразу после has_saved
                }
                state_.store(ClientState::JoinOk);
                joined_.store(true);
                isHost_.store(jr.isHost);
                lastPongMs = NowMs();
                if (onJoinOk_) onJoinOk_(jr);
                break;
            }
            case net::Msg::JoinRejected:
            {
                const uint8_t reason = p.empty() ? 0u : p[0];
                if (onJoinRejected_)
                    onJoinRejected_(static_cast<net::JoinRejectReason>(reason));
                // Несовпадение версии и «плохой ник» — фатальны (повторять
                // бессмысленно, надо править ini). Полный сервер/занятый ник
                // — временные: уходим в reconnect с backoff.
                const bool fatal =
                    reason == static_cast<uint8_t>(net::JoinRejectReason::VersionMismatch) ||
                    reason == static_cast<uint8_t>(net::JoinRejectReason::BadName);
                if (fatal)
                {
                    state_.store(ClientState::Error);
                    running_.store(false);
                }
                else
                {
                    reconnectRequested = true;
                }
                break;
            }
            case net::Msg::WorldSnapshot:
            {
                // P1: payload разбирает coop в игровом потоке; здесь проверяем
                // границы/конечность координат и передаём копию.
                if (IsSaneWorldSnapshot(p))
                {
                    if (onWorldSnapshot_) onWorldSnapshot_(p);
                }
                break;
            }
            case net::Msg::ActionState:
            {
                if (p.size() >= sizeof(net::ActionStatePayload))
                {
                    net::ActionStatePayload st{};
                    std::memcpy(&st, p.data(), sizeof(st));
                    if (onActionState_) onActionState_(st);
                }
                break;
            }
            case net::Msg::PlayerVehicle:
            {
                if (p.size() >= sizeof(net::PlayerVehicleState))
                {
                    net::PlayerVehicleState st{};
                    std::memcpy(&st, p.data(), sizeof(st));
                    if (IsSaneVehicle(st) && onPlayerVehicle_) onPlayerVehicle_(st);
                }
                break;
            }
            case net::Msg::MissionEvent:
            {
                if (p.size() >= sizeof(net::MissionEventPayload))
                {
                    net::MissionEventPayload ev{};
                    std::memcpy(&ev, p.data(), sizeof(ev));
                    if (onMissionEvent_) onMissionEvent_(ev);
                }
                break;
            }
            case net::Msg::MissionState:
            {
                if (p.size() >= sizeof(net::MissionStatePayload))
                {
                    net::MissionStatePayload st{};
                    std::memcpy(&st, p.data(), sizeof(st));
                    if (onMissionState_) onMissionState_(st);
                }
                break;
            }
            case net::Msg::MissionBarrier:
            {
                if (p.size() == sizeof(net::MissionBarrierPayload))
                {
                    net::MissionBarrierPayload ev{};
                    std::memcpy(&ev, p.data(), sizeof(ev));
                    if (onMissionBarrier_) onMissionBarrier_(ev);
                }
                break;
            }
            case net::Msg::TrainSnapshot:
            {
                // Сырой payload: проверку размера/carCount делает world_sync.
                if (onTrainSnapshot_) onTrainSnapshot_(p);
                break;
            }
            // ---------------- E0 (v15): actor-сообщения ----------------
            case net::Msg::ActorSpawnBatch:
            {
                uint32_t epoch = 0;
                std::vector<net::ActorSpawn> actors;
                if (ParseActorSpawnBatch(p, epoch, actors) && onActorSpawnBatch_)
                    onActorSpawnBatch_(epoch, actors);
                break;
            }
            case net::Msg::ActorFrames:
            {
                uint32_t epoch = 0;
                uint16_t seq = 0;
                std::vector<net::ActorFrame> frames;
                if (ParseActorFrames(p, epoch, seq, frames) && onActorFrames_)
                    onActorFrames_(epoch, seq, frames);
                break;
            }
            case net::Msg::ActorDespawn:
            {
                net::ActorDespawnPayload msg{};
                if (ParseActorDespawn(p, msg) && onActorDespawn_)
                    onActorDespawn_(msg);
                break;
            }
            case net::Msg::ActorEvent:
            {
                net::ActorEventPayload ev{};
                if (ParseActorEvent(p, ev) && onActorEvent_)
                    onActorEvent_(ev);
                break;
            }
            case net::Msg::ActorOutfit:
            {
                uint32_t epoch = 0, actorId = 0;
                net::OutfitState outfit{};
                if (ParseActorOutfit(p, epoch, actorId, outfit) && onActorOutfit_)
                    onActorOutfit_(epoch, actorId, outfit);
                break;
            }
            case net::Msg::ActorAppearance:
            {
                uint32_t epoch = 0, actorId = 0;
                std::vector<net::ActorAppearanceEntry> entries;
                if (ParseActorAppearance(p, epoch, actorId, entries) &&
                    onActorAppearance_)
                    onActorAppearance_(epoch, actorId, entries);
                break;
            }
            case net::Msg::ActorResync:
            {
                net::ActorResyncPayload req{};
                if (ParseActorResync(p, req) && onActorResyncRequest_)
                    onActorResyncRequest_(req);
                break;
            }
            case net::Msg::ActorDamage:
            {
                net::ActorDamagePayload req{};
                if (ParseActorDamage(p, req) && onActorDamageRequest_)
                    onActorDamageRequest_(req.epoch, req.actorId, req.damage,
                                          req.weaponHash);
                break;
            }
            case net::Msg::PlayerPresence:
            {
                net::PlayerPresencePayload req{};
                if (ParsePlayerPresence(p, req) && onPlayerPresence_)
                    onPlayerPresence_(req.epoch, req.flags);
                break;
            }
            case net::Msg::VehicleOccupants:
            {
                uint32_t vid = 0;
                std::vector<net::VehicleOccupantEntry> entries;
                if (ParseVehicleOccupants(p, vid, entries) &&
                    onVehicleOccupants_)
                    onVehicleOccupants_(vid, entries);
                break;
            }
            case net::Msg::TrainSnapshotV2:
            {
                net::TrainSnapshotV2Payload ts{};
                if (ParseTrainSnapshotV2(p, ts) && onTrainSnapshotV2_)
                    onTrainSnapshotV2_(ts);
                break;
            }
            case net::Msg::PlayerJoined:
            {
                PlayerJoinInfo ji;
                if (!ParseNamePayload(p, ji.name)) break;
                std::size_t off = 1u + static_cast<std::size_t>(p[0]);
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
                    if (ji.hasPos && (!IsFiniteCoord(ji.x) || !IsFiniteCoord(ji.y) ||
                                      !IsFiniteCoord(ji.z) || !IsFiniteCoord(ji.heading)))
                    {
                        ji.hasPos = false; // битая позиция — спавн рядом с хостом
                    }
                }
                if (onPlayerJoin_) onPlayerJoin_(ji);
                break;
            }
            case net::Msg::Frame:
            {
                if (p.size() >= sizeof(net::FramePayload))
                {
                    net::FramePayload fp;
                    std::memcpy(&fp, p.data(), sizeof(fp));
                    if (IsSaneFrame(fp) && onFrame_) onFrame_(fp);
                }
                break;
            }
            case net::Msg::PlayerLeft:
            {
                std::string nm;
                if (ParseNamePayload(p, nm))
                {
                    if (onPlayerLeave_) onPlayerLeave_(nm);
                }
                break;
            }
            case net::Msg::Died:
            {
                // От сервера: игрок умер. name в payload (как PlayerLeft).
                std::string nm;
                if (ParseNamePayload(p, nm))
                {
                    if (onDied_) onDied_(nm);
                }
                break;
            }
            case net::Msg::Respawned:
            {
                // От сервера: игрок ожил после смерти (name в payload).
                std::string nm;
                if (ParseNamePayload(p, nm))
                {
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
                    if (IsSaneWorldState(ws) && onWorldState_) onWorldState_(ws);
                }
                break;
            }
            case net::Msg::Outfit:
            {
                if (p.size() >= sizeof(net::OutfitState))
                {
                    net::OutfitState os;
                    std::memcpy(&os, p.data(), sizeof(os));
                    if (os.count > static_cast<uint32_t>(net::kMaxOutfitComponents))
                        os.count = static_cast<uint32_t>(net::kMaxOutfitComponents);
                    if (onOutfit_) onOutfit_(os);
                }
                break;
            }
            case net::Msg::HorseOutfit:
            {
                if (p.size() >= sizeof(net::OutfitState))
                {
                    net::OutfitState os;
                    std::memcpy(&os, p.data(), sizeof(os));
                    if (os.count > static_cast<uint32_t>(net::kMaxOutfitComponents))
                        os.count = static_cast<uint32_t>(net::kMaxOutfitComponents);
                    if (onHorseOutfit_) onHorseOutfit_(os);
                }
                break;
            }
            case net::Msg::Shot:
            {
                if (p.size() >= sizeof(net::ShotPayload))
                {
                    net::ShotPayload sp;
                    std::memcpy(&sp, p.data(), sizeof(sp));
                    if (IsFiniteCoord(sp.x) && IsFiniteCoord(sp.y) && IsFiniteCoord(sp.z) &&
                        onShot_)
                        onShot_(sp);
                }
                break;
            }
            case net::Msg::HostChanged:
            {
                if (!p.empty())
                {
                    const bool nowHost = p[0] != 0;
                    isHost_.store(nowHost);
                    if (onHostChanged_) onHostChanged_(nowHost);
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
                    const uint64_t now = NowMs();
                    if (now >= ts)
                        rttMs_.store(static_cast<uint32_t>(now - ts));
                }
                lastPongMs = NowMs(); // Pong — признак живого сервера
                break;
            }
            default:
                break;
            }

            if (!running_.load() || reconnectRequested) break;
        }
        if (!running_.load() || reconnectRequested) break;
    }

    // Снимаем публикацию текущего сокета и закрываем его. Другие потоки
    // могут держать shared_ptr — объект живёт до последнего владельца.
    {
        std::lock_guard<std::mutex> lk(sockMtx_);
        if (sock_ == sock) sock_.reset();
    }
    sock->Close();
    return false;
}

} // namespace netio
