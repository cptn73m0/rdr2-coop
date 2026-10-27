#define _CRT_SECURE_NO_WARNINGS 1
#include "config.h"
#include "log.h"

#include <windows.h>

#include <cctype>
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
    fputs("# E3d-a: репликация транспорта хоста (vehicle-only, без world_sync).\n", f);
    fputs("world_vehicles = 1\n", f);
    fputs("# E3d-a: радиус сканирования транспорта у хоста, м (20..200).\n", f);
    fputs("npc_vehicle_radius = 80\n\n", f);
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
    fputs("# R&D E0: каркас NPC actor sync (0 = выключено).\n", f);
    fputs("npc_actor = 0\n", f);
    fputs("# R&D E-1: probe-лог 20 ближайших human NPC (только при npc_actor=1).\n", f);
    fputs("npc_debug = 0\n", f);
    fputs("# E1: максимум NPC-акторов (1..12) и радиус обнаружения вокруг игроков (м).\n", f);
    fputs("npc_cap = 8\n", f);
    fputs("npc_radius = 60\n", f);
    fputs("# E3c: максимум животных и бесхозных лошадей (0..12; 0 = выключено).\n", f);
    fputs("npc_animal_cap = 4\n", f);
    fputs("# E5.2: верхний предел удержания трупа, сек (0..600; 0 = не держать).\n", f);
    fputs("npc_corpse_max_sec = 300\n", f);
    fputs("# E2a: meta-outfit внешность NPC-прокси (0 = только random variation).\n", f);
    fputs("npc_appearance = 1\n", f);
    fputs("# E5: host-authoritative урон по NPC/животным (0 = выключить урон).\n", f);
    fputs("npc_damage = 1\n", f);
    fputs("# E6: UI-статус игрока (карта/пауза/сатчель/магазин) в имени (0 = выкл).\n", f);
    fputs("npc_presence = 1\n", f);
    fputs("# E7.1: гость подавляет свою ambient-популяцию (0 = выключено).\n", f);
    fputs("npc_visitor = 1\n", f);
    fputs("# E7.1: подавлять локальный трафик у гостя (default 0: E3d-a ещё нет).\n", f);
    fputs("npc_visitor_vehicles = 1\n", f);
    fputs("# E7.1: сохранять сценарных/событийных NPC (vendors/quest/events).\n", f);
    fputs("npc_visitor_preserve_events = 1\n", f);
    fputs("# E7.1: паузить cleanup толпы на время миссии/катсцены (default 1).\n", f);
    fputs("npc_visitor_pause_scenes = 1\n", f);
    fputs("# E3d-c: пассажиры/водители/тягловые лошади (0 = выкл).\n", f);
    fputs("npc_occupants = 1\n", f);
    fputs("# E9: внешность животных/лошадей (0 = random variation).\n", f);
    fputs("npc_animal_appearance = 0\n", f);
    fputs("# E10: внешность лошади под NPC-всадником (0 = random).\n", f);
    fputs("npc_mount_appearance = 0\n", f);
    fputs("# P2: частота кадров actor-репликации, Гц (10..20; 10 = старое).\n", f);
    fputs("npc_frame_hz = 15\n", f);
    fputs("# P2: adaptive — стоя 10 Гц, при движении npc_frame_hz (default 1).\n", f);
    fputs("npc_frame_adaptive = 1\n", f);
    fputs("# P0: Player Frame throttle (10..60 Гц); change_only=1 -> 5 Гц в покое.\n", f);
    fputs("player_frame_hz = 20\n", f);
    fputs("player_frame_change_only = 1\n", f);
    fputs("# E7.2: in-place update go-to задачи (A/B; 0 = только far-target).\n", f);
    fputs("npc_motion_update = 1\n", f);
    fputs("# E7.2: fallback A2 marker-entity follow (default 0, после A/B).\n", f);
    fputs("npc_motion_marker = 0\n", f);
    fputs("# E7.2: p2 в GET_SCRIPT_TASK_STATUS (A/B, default 1).\n", f);
    fputs("npc_motion_status_p2 = 1\n", f);
    fclose(f);
}

bool AsBool(const std::string& val)
{
    std::string v;
    v.reserve(val.size());
    for (char c : val)
        v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return !(v == "0" || v == "false" || v == "no" || v == "off");
}

// ub-mode unit D: ApplyLine разделён на группы ключей; поведение то же.
struct IntKey
{
    const char* key;
    int Config::*field;
    int minValue;
    int maxValue;
    bool clamp; // true: clamp в диапазон; false: невалидное значение игнор
};

struct BoolKey
{
    const char* key;
    bool Config::*field;
};

bool ApplyIntKey(const std::string& key, const std::string& val, Config& out)
{
    static const IntKey keys[] = {
        {"server_port", &Config::serverPort, 1, 65535, false},
        {"npc_vehicle_radius", &Config::npcVehicleRadius, 20, 200, false},
        {"npc_cap", &Config::npcCap, 1, 12, false},
        {"npc_radius", &Config::npcRadius, 20, 100, false},
        {"npc_animal_cap", &Config::npcAnimalCap, 0, 12, false},
        {"npc_frame_hz", &Config::npcFrameHz, 10, 20, false},
        {"player_frame_hz", &Config::playerFrameHz, 10, 60, true},
        {"npc_corpse_max_sec", &Config::npcCorpseMaxSec, 0, 600, false},
    };
    for (const IntKey& k : keys)
    {
        if (key != k.key)
            continue;
        char* end = nullptr;
        const long v = std::strtol(val.c_str(), &end, 10);
        if (end == val.c_str() || *end != '\0')
            return true;
        if (k.clamp)
        {
            if (v < k.minValue) out.*(k.field) = k.minValue;
            else if (v > k.maxValue) out.*(k.field) = k.maxValue;
            else out.*(k.field) = static_cast<int>(v);
        }
        else if (v >= k.minValue && v <= k.maxValue)
        {
            out.*(k.field) = static_cast<int>(v);
        }
        return true;
    }
    return false;
}

bool ApplyBoolKey(const std::string& key, const std::string& val, Config& out)
{
    static const BoolKey keys[] = {
        {"world_sync", &Config::worldSync},
        {"world_vehicles", &Config::worldVehicles},
        {"action_sync", &Config::actionSync},
        {"vehicle_sync", &Config::vehicleSync},
        {"mission_sync", &Config::missionSync},
        {"mission_credit", &Config::missionCredit},
        {"save_backup", &Config::saveBackup},
        {"mission_reward_log", &Config::missionRewardLog},
        {"world_trains", &Config::worldTrains},
        {"train_debug", &Config::trainDebug},
        {"npc_actor", &Config::npcActor},
        {"npc_debug", &Config::npcDebug},
        {"npc_appearance", &Config::npcAppearance},
        {"npc_damage", &Config::npcDamage},
        {"npc_presence", &Config::npcPresence},
        {"npc_visitor", &Config::npcVisitor},
        {"npc_visitor_vehicles", &Config::npcVisitorVehicles},
        {"npc_visitor_preserve_events", &Config::npcVisitorPreserveEvents},
        {"npc_visitor_pause_scenes", &Config::npcVisitorPauseScenes},
        {"npc_occupants", &Config::npcOccupants},
        {"npc_animal_appearance", &Config::npcAnimalAppearance},
        {"npc_mount_appearance", &Config::npcMountAppearance},
        {"npc_frame_adaptive", &Config::npcFrameAdaptive},
        {"player_frame_change_only", &Config::playerFrameChangeOnly},
        {"npc_motion_update", &Config::npcMotionUpdate},
        {"npc_motion_marker", &Config::npcMotionMarker},
        {"npc_motion_status_p2", &Config::npcMotionStatusP2},
    };
    for (const BoolKey& k : keys)
    {
        if (key != k.key)
            continue;
        out.*(k.field) = AsBool(val);
        return true;
    }
    return false;
}

void ApplyLine(const std::string& raw, Config& out)
{
    std::string s = Trim(raw);
    if (s.empty() || s[0] == '#' || s[0] == ';') return;

    const size_t eq = s.find('=');
    if (eq == std::string::npos) return;

    const std::string key = Trim(s.substr(0, eq));
    // Inline-комментарии отрезаем только если перед '#'/';' стоит пробел/таб:
    // так "server_host = 1.2.3.4  ; сервер" работает, а ник "John#1" не ломается.
    std::string val = Trim(s.substr(eq + 1));
    for (size_t i = 0; i < val.size(); ++i)
    {
        if ((val[i] == '#' || val[i] == ';') && i > 0 &&
            (val[i - 1] == ' ' || val[i - 1] == '\t'))
        {
            val = Trim(val.substr(0, i));
            break;
        }
    }
    if (val.empty()) return;

    if (key == "server_host") out.serverHost = val;
    else if (key == "player_name") out.playerName = val;
    else if (ApplyIntKey(key, val, out)) return;
    else if (ApplyBoolKey(key, val, out)) return;
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

void WarnDangerousCombos()
{
    const Config& c = g_cfg;
    if (c.worldSync && c.npcActor)
        sh_log::Write("config: warn world_sync=1 && npc_actor=1: "
                      "возможны дубли world-snapshot/NPC (world_sync старше).\n");
    if (c.worldSync && c.worldTrains)
        sh_log::Write("config: warn world_sync=1 && world_trains=1: "
                      "старый train R&D (0x17) вместо mission-train 0x22.\n");
    if (!c.worldVehicles && (c.npcOccupants || c.npcVisitorVehicles))
        sh_log::Write("config: warn world_vehicles=0 with npc_occupants/"
                      "npc_visitor_vehicles=1: транспорт/occupants не появятся.\n");
    if (!c.npcActor && (c.worldVehicles || c.npcVisitor))
        sh_log::Write("config: warn npc_actor=0 with world_vehicles/npc_visitor=1: "
                      "vehicle/visitor-режим не активируется.\n");
}

} // namespace coopcfg
