#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace sh_log {

namespace {
FILE* g_f = nullptr;
}

void Init()
{
    if (g_f) return;
    // Resolve the directory of the plugin module (the game's scripts\)
    // so the log is found regardless of the process CWD.
    wchar_t path[MAX_PATH] = {0};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Init, &self);
    if (!self) return;
    GetModuleFileNameW(self, path, MAX_PATH);

    // Trim the module file name, keep the directory.
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t* fslash = wcsrchr(path, L'/');
    wchar_t* last = (slash && (!fslash || slash > fslash)) ? slash : fslash;
    if (last) last[1] = L'\0';
    wcsncat(path, L"rdr2-coop.log", MAX_PATH - wcslen(path) - 1);

    g_f = _wfopen(path, L"a");
}

void Write(const char* fmt, ...)
{
    if (!g_f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_f, fmt, args);
    va_end(args);
    fflush(g_f);
}

} // namespace sh_log