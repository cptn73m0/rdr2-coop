#pragma once

// Shared logging into the plugin's own directory (scripts\) so that both
// the bootstrap (main.cpp) and the coop prototype (coop.cpp) write to one
// stable file, independent of the game process CWD.

namespace sh_log {

// Initialize the log file next to the .asi (scripts\rdr2-coop.log).
// Safe to call more than once: only the first call opens the file.
void Init();

// Append a line. No-op if Init() was not called.
void Write(const char* fmt, ...);

} // namespace sh_log