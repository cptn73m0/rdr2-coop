#pragma once

#include "native_call.h"

// Phase-1 local prototype: spawn a "ghost" ped alongside the player and
// drive its transform by the player transform, proving the engine will
// create and animate a second entity that we read/write each frame.
// No network yet — this is the foundation the network layer will feed later.

namespace coop {

// Called every script tick; idempotent against repeated entry into the
// world (Player 0 ped may be re-created).
void Tick();

} // namespace coop