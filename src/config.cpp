#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coopcfg {

namespace {

Config g_cfg;

// Папка, где лежит сам плагин (scripts\).
std::wstring PluginDir()
{
    wchar_t path[MAX_PATH] = {0};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&PluginDir, &self);
    if (!self) return L"";
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wchar_t* fslash = wcsrchr(path, L'/');
    wchar_t* last = (slash && (!fslash || slash > fslash)) ? slash : fslash;
    if (last) last[1] = L'\0';
    return std::wstring(path);
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring IniPath()
{
    return PluginDir() + L"rdr2-coop.ini";
}

void WriteDefaultIni()
{
    FILE* f = _wfopen(IniPath().c_str(), L"w");
    if (!f) return;
    fputs("# RDR2 CO-OP — настройки клиента\n", f);
    fputs("# Адрес и порт сервера-релея:\n", f);
    fputs("server_host = 127.0.0.1\n", f);
    fputs("server_port = 7777\n\n", f);
    fputs("# Ваш ник в игре (виден другим игрокам):\n", f);
    fputs("player_name = Player\n\n", f);
    fputs("# Модель вашего персонажа (хэш). Примеры:\n", f);
    fputs("#   a_m_m_armtownfolk_01 = 0xCF7E73BE (горожанин)\n", f);
    fputs("#   a_c_dogstreet_01      = 0x3B313FCE (собака)\n", f);
    fputs("ped_model = 0xCF7E73BE\n", f);
    fclose(f);
}

} // namespace

Config& Load()
{
    // Если файла нет — создать шаблон с дефолтами.
    FILE* probe = _wfopen(IniPath().c_str(), L"r");
    if (!probe)
    {
        WriteDefaultIni();
        return g_cfg;
    }
    fclose(probe);

    FILE* f = _wfopen(IniPath().c_str(), L"r");
    if (!f) return g_cfg;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;

        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(s.substr(0, eq));
        std::string val = Trim(s.substr(eq + 1));
        if (val.empty()) continue;

        if (key == "server_host") g_cfg.serverHost = val;
        else if (key == "server_port") g_cfg.serverPort = std::atoi(val.c_str());
        else if (key == "player_name") g_cfg.playerName = val;
        else if (key == "ped_model")
        {
            // hex (0x...) или десятичное
            unsigned long long m = 0;
            if (val.size() > 2 && (val[0] == '0') && (val[1] == 'x' || val[1] == 'X'))
                m = std::strtoull(val.c_str() + 2, nullptr, 16);
            else
                m = std::strtoull(val.c_str(), nullptr, 10);
            if (m) g_cfg.pedModel = m;
        }
    }
    fclose(f);
    return g_cfg;
}

Config& Get()
{
    return g_cfg;
}

} // namespace coopcfg