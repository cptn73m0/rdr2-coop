// v20 P1 (U1): protocol-level tests for VehicleInteraction (0x23),
// VehicleInteractionResult (0x24), VehicleState (0x26) and their inline
// validators. Behavior-level: each test calls the same validators the
// server/client use and checks the observable accept/reject result.

#include "net/protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

int g_failures = 0;

template <typename T>
std::vector<uint8_t> BytesOf(const T& value)
{
    std::vector<uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

void Expect(bool cond, const char* what)
{
    if (!cond)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

net::VehicleInteractionPayload GoodInteraction()
{
    net::VehicleInteractionPayload v{};
    v.epoch = 7;
    v.vehicleNetId = 42;
    v.targetId = 0;
    v.seq = 3;
    v.action = static_cast<uint8_t>(net::VehicleAction::Damage);
    v.seat = net::kVehicleSeatNone;
    v.door = net::kVehiclePartWheelFL;
    v.flags = 0;
    v.param = 60;
    v.weaponHash = 0xAABBCCDDu;
    return v;
}

net::VehicleInteractionResultPayload GoodResult()
{
    net::VehicleInteractionResultPayload r{};
    r.epoch = 7;
    r.vehicleNetId = 42;
    r.seq = 3;
    r.action = static_cast<uint8_t>(net::VehicleAction::Damage);
    r.result = static_cast<uint8_t>(net::VehicleResult::Applied);
    r.detail = 0;
    return r;
}

net::VehicleStatePayload GoodState()
{
    net::VehicleStatePayload s{};
    s.epoch = 7;
    s.vehicleNetId = 42;
    s.bodyHealth = 500;
    s.engineHealth = 500;
    s.doorBits = 0x0001;
    s.seq = 9;
    s.flags = net::kVehicleStateFlagDoorValid;
    s.wheelsBits = 0;
    s.hostSeat = net::kVehicleSeatNone;
    s.guestSeat = net::kVehicleSeatNone;
    s.reserved = 0;
    return s;
}

void TestLayoutAndEnums()
{
    static_assert(sizeof(net::VehicleInteractionPayload) == 24, "0x23 size");
    static_assert(sizeof(net::VehicleInteractionResultPayload) == 16, "0x24 size");
    static_assert(sizeof(net::VehicleStatePayload) == 24, "0x26 size");
    static_assert(offsetof(net::VehicleInteractionPayload, action) == 14, "action off");
    static_assert(offsetof(net::VehicleInteractionPayload, weaponHash) == 20, "weapon off");
    static_assert(std::is_trivially_copyable<net::VehicleInteractionPayload>::value,
                  "0x23 trivial");
    static_assert(std::is_trivially_copyable<net::VehicleInteractionResultPayload>::value,
                  "0x24 trivial");
    static_assert(std::is_trivially_copyable<net::VehicleStatePayload>::value,
                  "0x26 trivial");

    Expect(static_cast<uint8_t>(net::Msg::VehicleInteraction) == 0x23, "0x23 id");
    Expect(static_cast<uint8_t>(net::Msg::VehicleInteractionResult) == 0x24, "0x24 id");
    Expect(static_cast<uint8_t>(net::Msg::VehicleControlInput) == 0x25, "0x25 reserved");
    Expect(static_cast<uint8_t>(net::Msg::VehicleState) == 0x26, "0x26 id");
    Expect(static_cast<uint8_t>(net::Msg::VehicleControlState) == 0x27, "0x27 reserved");
    Expect(net::kProtocolVersion == 21,
           "protocol version 21 (v21 actor-cap bump)");

    Expect(static_cast<uint8_t>(net::VehicleAction::Damage) == 1, "action Damage=1");
    Expect(static_cast<uint8_t>(net::VehicleAction::DoorOpen) == 2, "DoorOpen=2");
    Expect(static_cast<uint8_t>(net::VehicleAction::DoorClose) == 3, "DoorClose=3");
    Expect(static_cast<uint8_t>(net::VehicleAction::DoorLock) == 4, "DoorLock=4");
    Expect(static_cast<uint8_t>(net::VehicleAction::DoorUnlock) == 5, "DoorUnlock=5");
    Expect(static_cast<uint8_t>(net::VehicleAction::DoorBreak) == 6, "DoorBreak=6");
    Expect(static_cast<uint8_t>(net::VehicleAction::Enter) == 7, "Enter=7");
    Expect(static_cast<uint8_t>(net::VehicleAction::Exit) == 8, "Exit=8");
    Expect(static_cast<uint8_t>(net::VehicleAction::Hijack) == 9, "Hijack=9");
    Expect(static_cast<uint8_t>(net::VehicleAction::Loot) == 10, "Loot=10");
    Expect(static_cast<uint8_t>(net::VehicleAction::Hitch) == 11, "Hitch=11");
    Expect(static_cast<uint8_t>(net::VehicleAction::Unhitch) == 12, "Unhitch=12");

    Expect(static_cast<uint8_t>(net::VehicleResult::Applied) == 0, "result Applied=0");
    Expect(static_cast<uint8_t>(net::VehicleResult::NotAllowed) == 1, "NotAllowed=1");
    Expect(static_cast<uint8_t>(net::VehicleResult::StaleEpoch) == 2, "StaleEpoch=2");
    Expect(static_cast<uint8_t>(net::VehicleResult::NotFound) == 3, "NotFound=3");
    Expect(static_cast<uint8_t>(net::VehicleResult::OutOfRange) == 4, "OutOfRange=4");
    Expect(static_cast<uint8_t>(net::VehicleResult::SeatOccupied) == 5, "SeatOccupied=5");
    Expect(static_cast<uint8_t>(net::VehicleResult::Locked) == 6, "Locked=6");
    Expect(static_cast<uint8_t>(net::VehicleResult::RateLimited) == 7, "RateLimited=7");
    Expect(static_cast<uint8_t>(net::VehicleResult::Timeout) == 8, "Timeout=8");
    Expect(static_cast<uint8_t>(net::VehicleResult::Correction) == 9, "Correction=9");

    const uint32_t detail = (static_cast<uint32_t>(net::kResultReasonMissionProtected) << 8) | 0x12;
    Expect(net::VehicleInteractionReason(detail) == net::kResultReasonMissionProtected,
           "reason bits detail>>8");
    Expect(net::kInteractionKnownFlags == 0x3F, "interaction known flags 0x3F");
    Expect(net::kVehicleStateKnownFlags == 0xFF, "state known flags 0xFF");
}

void TestInteractionValid()
{
    Expect(net::IsValidVehicleInteraction(BytesOf(GoodInteraction())), "good Damage");

    net::VehicleInteractionPayload v = GoodInteraction();
    v.action = static_cast<uint8_t>(net::VehicleAction::Enter);
    v.seat = net::kVehicleSeatDriver;
    v.door = net::kVehicleDoorNone;
    v.param = 0;
    v.weaponHash = 0;
    Expect(net::IsValidVehicleInteraction(BytesOf(v)), "good Enter driver");

    v.action = static_cast<uint8_t>(net::VehicleAction::DoorOpen);
    v.door = 3;
    Expect(net::IsValidVehicleInteraction(BytesOf(v)), "good DoorOpen");

    v.action = static_cast<uint8_t>(net::VehicleAction::DoorLock);
    v.door = net::kVehicleDoorAll;
    Expect(net::IsValidVehicleInteraction(BytesOf(v)), "good DoorLock all");

    v.action = static_cast<uint8_t>(net::VehicleAction::Hijack);
    v.seat = net::kVehicleSeatDriver;
    v.door = net::kVehicleDoorNone;
    v.flags = net::kInteractionTargetActor;
    v.targetId = 77;
    Expect(net::IsValidVehicleInteraction(BytesOf(v)), "good Hijack actor target");

    v.action = static_cast<uint8_t>(net::VehicleAction::Loot);
    v.seat = net::kVehicleSeatNone;
    v.flags = net::kInteractionTargetEntity;
    v.targetId = 88;
    Expect(net::IsValidVehicleInteraction(BytesOf(v)), "good Loot entity target");

    v.action = static_cast<uint8_t>(net::VehicleAction::Hitch);
    v.door = 3;
    v.flags = 0;
    v.targetId = 0;
    Expect(net::IsValidVehicleInteraction(BytesOf(v)), "good Hitch");
}

void TestInteractionInvalid()
{
    net::VehicleInteractionPayload v = GoodInteraction();
    auto bad = [&v](const char* what) {
        Expect(!net::IsValidVehicleInteraction(BytesOf(v)), what);
    };

    std::vector<uint8_t> bytes = BytesOf(v);
    bytes.pop_back();
    Expect(!net::IsValidVehicleInteraction(bytes), "short 0x23 rejected");
    bytes = BytesOf(v);
    bytes.push_back(0);
    Expect(!net::IsValidVehicleInteraction(bytes), "long 0x23 rejected");

    v = GoodInteraction(); v.epoch = 0; bad("epoch=0 rejected");
    v = GoodInteraction(); v.vehicleNetId = 0; bad("vehicleNetId=0 rejected");
    v = GoodInteraction(); v.seq = 0; bad("seq=0 rejected");
    v = GoodInteraction(); v.action = 0; bad("action None rejected");
    v = GoodInteraction(); v.action = 13; bad("action >12 rejected");
    v = GoodInteraction(); v.flags = 0x40; bad("unknown flags rejected");
    v = GoodInteraction(); v.flags = net::kInteractionTargetActor | net::kInteractionTargetEntity;
    bad("Actor+Entity flags rejected");
    v = GoodInteraction(); v.seat = 9; bad("seat 9 rejected");
    v = GoodInteraction(); v.seat = -2; bad("seat -2 rejected");
    v = GoodInteraction(); v.door = 12; bad("Damage window part 12 rejected");
    v = GoodInteraction(); v.door = 15; bad("Damage window part 15 rejected");
    v = GoodInteraction(); v.param = 0; bad("Damage param 0 rejected");
    v = GoodInteraction(); v.param = 1001; bad("Damage param 1001 rejected");
    v = GoodInteraction(); v.weaponHash = 0; Expect(net::IsValidVehicleInteraction(BytesOf(v)),
                                                   "Damage weaponHash 0 ok");
    v = GoodInteraction();
    v.action = static_cast<uint8_t>(net::VehicleAction::DoorOpen);
    v.door = 8; v.param = 0; v.weaponHash = 0;
    bad("DoorOpen door 8 rejected");
    v.action = static_cast<uint8_t>(net::VehicleAction::DoorLock);
    v.door = 8; bad("DoorLock door 8 rejected");
    v.action = static_cast<uint8_t>(net::VehicleAction::Enter);
    v.door = 3; bad("Enter door!=255 rejected");
    v.action = static_cast<uint8_t>(net::VehicleAction::Exit);
    v.seat = 0; v.door = net::kVehicleDoorNone; bad("Exit seat!=127 rejected");
    v.action = static_cast<uint8_t>(net::VehicleAction::Hijack);
    v.seat = 0; v.door = net::kVehicleDoorNone; bad("Hijack seat 0 rejected");
    v.action = static_cast<uint8_t>(net::VehicleAction::Loot);
    v.seat = net::kVehicleSeatNone; v.flags = 0; v.targetId = 5;
    bad("Loot target without TargetEntity rejected");
    v.action = static_cast<uint8_t>(net::VehicleAction::Hitch);
    v.door = 6; v.param = 0; bad("Hitch harness 6 rejected");
    v = GoodInteraction();
    v.action = static_cast<uint8_t>(net::VehicleAction::DoorOpen);
    v.door = 1; v.param = 0; v.weaponHash = 1;
    bad("non-Damage weaponHash!=0 rejected");
}

void TestResultValidInvalid()
{
    Expect(net::IsValidVehicleInteractionResult(BytesOf(GoodResult())), "good result");
    for (uint8_t r = 0; r <= 9; ++r)
    {
        net::VehicleInteractionResultPayload v = GoodResult();
        v.result = r;
        Expect(net::IsValidVehicleInteractionResult(BytesOf(v)), "result 0..9 accepted");
    }
    net::VehicleInteractionResultPayload v = GoodResult();
    v.epoch = 0; Expect(!net::IsValidVehicleInteractionResult(BytesOf(v)), "result epoch0");
    v = GoodResult(); v.vehicleNetId = 0;
    Expect(!net::IsValidVehicleInteractionResult(BytesOf(v)), "result netId0");
    v = GoodResult(); v.seq = 0;
    Expect(!net::IsValidVehicleInteractionResult(BytesOf(v)), "result seq0");
    v = GoodResult(); v.action = 0;
    Expect(!net::IsValidVehicleInteractionResult(BytesOf(v)), "result action0");
    v = GoodResult(); v.action = 13;
    Expect(!net::IsValidVehicleInteractionResult(BytesOf(v)), "result action13");
    v = GoodResult(); v.result = 10;
    Expect(!net::IsValidVehicleInteractionResult(BytesOf(v)), "result 10 rejected");
    std::vector<uint8_t> bytes = BytesOf(GoodResult());
    bytes.pop_back();
    Expect(!net::IsValidVehicleInteractionResult(bytes), "short 0x24 rejected");
}

void TestStateValidInvalid()
{
    Expect(net::IsValidVehicleState(BytesOf(GoodState())), "good state");
    net::VehicleStatePayload s = GoodState();
    s.bodyHealth = net::kVehicleBodyHealthUnknown;
    s.engineHealth = net::kVehicleEngineHealthUnknown;
    s.hostSeat = net::kVehicleSeatDriver;
    s.guestSeat = 0;
    s.flags = net::kVehicleStateKnownFlags;
    Expect(net::IsValidVehicleState(BytesOf(s)), "state unknown health + full flags");

    s = GoodState(); s.epoch = 0;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state epoch0");
    s = GoodState(); s.vehicleNetId = 0;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state netId0");
    s = GoodState(); s.bodyHealth = 1001;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state bodyHealth 1001");
    s = GoodState(); s.engineHealth = -4001;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state engineHealth -4001");
    s = GoodState(); s.engineHealth = 1001;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state engineHealth 1001");
    s = GoodState(); s.hostSeat = 9;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state hostSeat 9");
    s = GoodState(); s.guestSeat = -2;
    Expect(!net::IsValidVehicleState(BytesOf(s)), "state guestSeat -2");
    std::vector<uint8_t> bytes = BytesOf(GoodState());
    bytes.pop_back();
    Expect(!net::IsValidVehicleState(bytes), "short 0x26 rejected");
}

void TestRoundTrip()
{
    const net::VehicleInteractionPayload in = GoodInteraction();
    const std::vector<uint8_t> wire = BytesOf(in);
    net::VehicleInteractionPayload out{};
    std::memcpy(&out, wire.data(), sizeof(out));
    Expect(out.epoch == in.epoch && out.vehicleNetId == in.vehicleNetId &&
           out.targetId == in.targetId && out.seq == in.seq &&
           out.action == in.action && out.seat == in.seat &&
           out.door == in.door && out.flags == in.flags &&
           out.param == in.param && out.weaponHash == in.weaponHash,
           "0x23 byte round-trip");
}

} // namespace

int main()
{
    TestLayoutAndEnums();
    TestInteractionValid();
    TestInteractionInvalid();
    TestResultValidInvalid();
    TestStateValidInvalid();
    TestRoundTrip();

    if (g_failures != 0)
    {
        std::printf("vehicle_interaction_protocol_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vehicle_interaction_protocol_test: PASS\n");
    return 0;
}
