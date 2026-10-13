#pragma once

// Minimal persistence of the remote player's last known world position,
// stored as a small JSON file next to the plugin. Used so that a returning
// player spawns where they were on their last leave, instead of next to us.
//
// Format (last_positions.json):
//   { "players": { "<name>": { "x": .., "y": .., "z": .., "heading": .. } } }

namespace persist {

// Load the saved position for `name`. Returns false if none is stored.
bool LoadLastPosition(const char* name, float& x, float& y, float& z, float& heading);

// Save/update the position for `name` and write the file.
void SaveLastPosition(const char* name, float x, float y, float z, float heading);

} // namespace persist