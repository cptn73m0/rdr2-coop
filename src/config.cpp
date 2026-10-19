#define _CRT_SECURE_NO_WARNINGS 1
#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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
    fputs("# Модель, которой вас увидят другие игроки (хэш).\n", f);
    fputs("# Примеры:\n", f);
    fputs("#   a_m_m_armtownfolk_01  = 0xCF7E73BE (горожанин)\n", f);
    fputs("#   a_f_m_armtownfolk_01  = 0x9854FB06 (горожанка)\n", f);
    fputs("#   a_m_m_rancher_01      = 0x54D8BE8E (ранчер)\n", f);
    fputs("#   a_c_dogstreet_01      = 0x3B313FCE (собака)\n", f);
    fputs("ped_model = 0xCF7E73BE\n\n", f);
    fputs("# P1: репликация мира/NPC (хост -> гость).\n", f);
    fputs("# 0 = выключено (по умолчанию), 1 = включено на ОБОИХ ПК.\n", f);
    fputs("world_sync = 0\n\n", f);
    fputs("# P2: Action v2 — сценарии/сидение/курение/укрытие.\n", f);
    fputs("# 0 = выключено (по умолчанию), 1 = включено на ОБОИХ ПК.\n", f);
    fputs("action_sync = 0\n", f);
    fputs("# P2: посадка в транспорт (дилижанс/повозка) удалённого игрока.\n", f);
    fputs("# 0 = выключено (по умолчанию), 1 = включено на ОБОИХ ПК.\n", f);
    fputs("vehicle_sync = 0\n", f);
    fputs("# P3: хост детектит новые завершённые миссии и шлёт гостю.\n", f);
    fputs("# 0 = выключено (по умолчанию), 1 = включено на ОБОИХ ПК.\n", f);
    fputs("mission_sync = 0\n", f);
    fputs("# P3: писать зачёт миссии в сейв гостя (только при равном прогрессе!).\n", f);
    fputs("mission_credit = 0\n", f);
    fputs("# P3: перед зачётом сделать бэкап сейва гостя (рекомендуется 1).\n", f);
    fputs("save_backup = 1\n", f);
    fputs("# P3: dry-run лог плана наград миссии (нативы не вызываются).\n", f);
    fputs("mission_reward_log = 1\n", f);
    fputs("# R&D: экспериментальная репликация поездов (состав). 0 = выкл.\n", f);
    fputs("world_trains = 0\n", f);
    fputs("# R&D: подробный лог offset/attach вагонов (диагностика).\n", f);
    fputs("train_debug = 0\n", f);
    fputs("# P1 safe: реплицировать mission-peds/objects (по умолчанию 0).\n", f);
    fputs("world_mission_entities = 0\n", f);
    fputs("# P1 safe: глушить ambient людей на госте (животные/транспорт — нет).\n", f);
    fputs("world_suppress_ambient = 0\n", f);
    fclose(f);
}

bool AsBool(const std::string& val)
{
    return !(val == "0" || val == "false" || val == "no" ||
             val == "off" || val == "False" || val == "OFF");
}

void ApplyLine(const std::string& raw, Config& out)
{
    const std::string s = Trim(raw);
    if (s.empty() || s[0] == '#' || s[0] == ';') return;

    const size_t eq = s.find('=');
    if (eq == std::string::npos) return;

    const std::string key = Trim(s.substr(0, eq));
    const std::string val = Trim(s.substr(eq + 1));
    if (val.empty()) return;

    if (key == "server_host") out.serverHost = val;
    else if (key == "server_port") out.serverPort = std::atoi(val.c_str());
    else if (key == "player_name") out.playerName = val;
    else if (key == "world_sync") out.worldSync = AsBool(val);
    else if (key == "action_sync") out.actionSync = AsBool(val);
    else if (key == "vehicle_sync") out.vehicleSync = AsBool(val);
    else if (key == "mission_sync") out.missionSync = AsBool(val);
    else if (key == "mission_credit") out.missionCredit = AsBool(val);
    else if (key == "save_backup") out.saveBackup = AsBool(val);
    else if (key == "mission_reward_log") out.missionRewardLog = AsBool(val);
    else if (key == "world_trains") out.worldTrains = AsBool(val);
    else if (key == "train_debug") out.trainDebug = AsBool(val);
    else if (key == "world_mission_entities") out.worldMissionEntities = AsBool(val);
    else if (key == "world_suppress_ambient") out.worldSuppressAmbient = AsBool(val);
    else if (key == "ped_model")
    {
        unsigned long long m = 0;
        if (val.size() > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))
            m = std::strtoull(val.c_str() + 2, nullptr, 16);
        else
            m = std::strtoull(val.c_str(), nullptr, 10);
        if (m) out.pedModel = m;
    }
}

bool ParseStream(std::istream& in, Config& out)
{
    std::string line;
    while (std::getline(in, line))
        ApplyLine(line, out);
    return true;
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

    std::ifstream f(IniPath());
    if (!f) return g_cfg;
    ParseStream(f, g_cfg);
    return g_cfg;
}

bool LoadFromFile(const std::string& path, Config& out)
{
    std::ifstream f(path);
    if (!f) return false;
    return ParseStream(f, out);
}

Config& Get()
{
    return g_cfg;
}

} // namespace coopcfg
