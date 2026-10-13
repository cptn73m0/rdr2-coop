#include "script_hook.h"

#include <string>
#include <vector>

namespace sh {

FnScriptRegister   scriptRegister = nullptr;
FnNativeInit       nativeInit     = nullptr;
FnNativePush64     nativePush64   = nullptr;
FnNativeCall       nativeCall     = nullptr;
FnScriptWait       scriptWait     = nullptr;

namespace {

// Find the address of an export whose C/C++ name contains `token`.
// ScriptHookRDR2 V2 (and in some builds the classic one) exports functions
// under mangled C++ names like "?scriptRegister@@YAX..."; the classic
// Alexander-Blade build exports plain "scriptRegister". Searching the PE
// export directory and matching a token covers both.
void* FindExport(HMODULE mod, const char* token)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ed.VirtualAddress || !ed.Size) return nullptr;

    const IMAGE_EXPORT_DIRECTORY* exp =
        (const IMAGE_EXPORT_DIRECTORY*)((BYTE*)mod + ed.VirtualAddress);

    const DWORD* names = (const DWORD*)((BYTE*)mod + exp->AddressOfNames);
    const WORD* ordinals = (const WORD*)((BYTE*)mod + exp->AddressOfNameOrdinals);
    const DWORD* funcs = (const DWORD*)((BYTE*)mod + exp->AddressOfFunctions);

    size_t tokenLen = strlen(token);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i)
    {
        const char* name = (const char*)((BYTE*)mod + names[i]);
        if (strstr(name, token) != nullptr)
        {
            WORD ord = ordinals[i];
            return (BYTE*)mod + funcs[ord];
        }
    }
    return nullptr;
}

} // namespace

bool Init(LPCWSTR moduleName)
{
    HMODULE hook = GetModuleHandleW(moduleName);
    if (!hook)
        hook = LoadLibraryW(moduleName);
    if (!hook)
        return false;

    scriptRegister = (FnScriptRegister)FindExport(hook, "scriptRegister");
    nativeInit     = (FnNativeInit)FindExport(hook, "nativeInit");
    nativePush64   = (FnNativePush64)FindExport(hook, "nativePush64");
    nativeCall     = (FnNativeCall)FindExport(hook, "nativeCall");
    scriptWait     = (FnScriptWait)FindExport(hook, "scriptWait");

    return scriptRegister && nativeInit && nativePush64 && nativeCall && scriptWait;
}

} // namespace sh