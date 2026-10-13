#pragma once

// The shared state container for a synced remote actor ("ghost"). This is
// exactly what will be sent over the network later: one machine fills it
// from the friend's local player, the other applies it to the local ghost
// ped. For now it's filled/tested locally, but the struct is transport-
// neutral (plain data, ready to serialize).

#include <cstdint>

namespace coop {

// Current locomotion state of the source player (what animation the remote
// ped should be playing). Minimal for the container MVP.
enum class MoveState : uint8_t
{
    Idle = 0,
    Walk = 1,
    Run = 2,
    RideHorse = 3,
};

struct GhostFrame
{
    float    x = 0.f;          // world X
    float    y = 0.f;          // world Y
    float    z = 0.f;          // world Z
    float    heading = 0.f;    // compass heading (deg)
    MoveState move = MoveState::Idle;
    uint64_t seq = 0;          // frame sequence number (for interpolation)
};

} // namespace coop