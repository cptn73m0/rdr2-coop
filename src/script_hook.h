#pragma once

// Runtime resolution of the ScriptHookRDR2 plugin API.
//
// Modern RDR2 ASI plugins link against exports of ScriptHookRDR2.dll
// (the Alexander-Blade-compatible {scriptRegister, nativeInit,
// nativePush64, nativeCall} set). We resolve them via GetProcAddress so
// that no proprietary ScriptHookRDR2.lib / natives.h SDK is needed: the
// user supplies the released ScriptHookRDR2.dll (or V2) at runtime.

#include <windows.h>
#include <cstdint>

namespace sh {

using ScriptMain = void (*)();

// scriptRegister(HMODULE module, ScriptMain main) — register a script thread.
using FnScriptRegister = void (*)(HMODULE module, ScriptMain main);
// nativeInit(uint64_t hash) — begin an invocation of native `hash`.
using FnNativeInit = void (*)(uint64_t hash);
// nativePush64(uint64_t value) — push one arg into the native invocation.
using FnNativePush64 = void (*)(uint64_t value);
// nativeCall() -> uint64_t* — execute; high word of result / return in low.
using FnNativeCall = uint64_t* (*)();
// scriptWait(DWORD time) — yield the script thread for `time` ms.
using FnScriptWait = void (*)(DWORD time);
// keyboardHandlerRegister/Unregister — optional V2 keyboard input.
using KeyHandler = void (*)(DWORD key, bool down);
using FnKeyRegister   = void (*)(KeyHandler handler);
using FnKeyUnregister = void (*)(KeyHandler handler);

extern FnScriptRegister   scriptRegister;
extern FnNativeInit       nativeInit;
extern FnNativePush64     nativePush64;
extern FnNativeCall       nativeCall;
extern FnScriptWait       scriptWait;

// Load ScriptHookRDR2.dll exports. Call once before scriptRegister.
bool Init(LPCWSTR moduleName = L"ScriptHookRDR2.dll");

} // namespace sh