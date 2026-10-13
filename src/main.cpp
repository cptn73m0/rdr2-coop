// RDR2 CO-OP — bootstrap ASI plugin.
// Phase 0 goal: an empty .asi that registers with ScriptHookRDR2 and
// ticks a no-op. Later phases add transport sync, animation sync, and
// the ghost observer.

#include <windows.h>

#include "coop.h"
#include "log.h"
#include "script_hook.h"

namespace {

// Simple tick body — drives Phase-1 local prototype (spawn ghost ped,
// mirror player transform). Network feed lands here in later phases.
void TickBody()
{
    coop::Tick();
}

// Game-loop thread callback: called repeatedly by ScriptHookRDR2.
void ScriptMain()
{
    sh_log::Write("[rdr2-coop] ScriptMain entered\n");
    while (true)
    {
        TickBody();
        sh::scriptWait(0);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hMod);
        sh_log::Init();

        if (!sh::Init())
        {
            sh_log::Write("[rdr2-coop] ScriptHookRDR2.dll not found\n");
            return TRUE; // stay loaded; log explains absence
        }

        sh_log::Write("[rdr2-coop] registering with ScriptHookRDR2\n");
        sh::scriptRegister(hMod, &ScriptMain);
    }
    return TRUE;
}