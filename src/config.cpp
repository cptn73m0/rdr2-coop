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
    else if (key == "server_port")
    {
        char* end = nullptr;
        const long port = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && port >= 1 && port <= 65535)
            out.serverPort = static_cast<int>(port);
    }
    else if (key == "player_name") out.playerName = val;
    else if (key == "world_sync") out.worldSync = AsBool(val);
    else if (key == "world_vehicles") out.worldVehicles = AsBool(val);
    else if (key == "npc_vehicle_radius")
    {
        char* end = nullptr;
        const long r = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && r >= 20 && r <= 200)
            out.npcVehicleRadius = static_cast<int>(r);
    }
    else if (key == "action_sync") out.actionSync = AsBool(val);
    else if (key == "vehicle_sync") out.vehicleSync = AsBool(val);
    else if (key == "mission_sync") out.missionSync = AsBool(val);
    else if (key == "mission_credit") out.missionCredit = AsBool(val);
    else if (key == "save_backup") out.saveBackup = AsBool(val);
    else if (key == "mission_reward_log") out.missionRewardLog = AsBool(val);
    else if (key == "world_trains") out.worldTrains = AsBool(val);
    else if (key == "train_debug") out.trainDebug = AsBool(val);
    else if (key == "npc_actor") out.npcActor = AsBool(val);
    else if (key == "npc_debug") out.npcDebug = AsBool(val);
    else if (key == "npc_cap")
    {
        char* end = nullptr;
        const long cap = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && cap >= 1 && cap <= 12)
            out.npcCap = static_cast<int>(cap);
    }
    else if (key == "npc_radius")
    {
        char* end = nullptr;
        const long r = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && r >= 20 && r <= 100)
            out.npcRadius = static_cast<int>(r);
    }
    else if (key == "npc_animal_cap")
    {
        char* end = nullptr;
        const long a = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && a >= 0 && a <= 12)
            out.npcAnimalCap = static_cast<int>(a);
    }
    else if (key == "npc_appearance") out.npcAppearance = AsBool(val);
    else if (key == "npc_damage") out.npcDamage = AsBool(val);
    else if (key == "npc_presence") out.npcPresence = AsBool(val);
    else if (key == "npc_visitor") out.npcVisitor = AsBool(val);
    else if (key == "npc_visitor_vehicles")
        out.npcVisitorVehicles = AsBool(val);
    else if (key == "npc_visitor_preserve_events")
        out.npcVisitorPreserveEvents = AsBool(val);
    else if (key == "npc_visitor_pause_scenes")
        out.npcVisitorPauseScenes = AsBool(val);
    else if (key == "npc_occupants") out.npcOccupants = AsBool(val);
    else if (key == "npc_animal_appearance")
        out.npcAnimalAppearance = AsBool(val);
    else if (key == "npc_mount_appearance")
        out.npcMountAppearance = AsBool(val);
    else if (key == "npc_frame_hz")
    {
        char* end = nullptr;
        const long hz = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && hz >= 10 && hz <= 20)
            out.npcFrameHz = static_cast<int>(hz);
    }
    else if (key == "npc_frame_adaptive")
        out.npcFrameAdaptive = AsBool(val);
    else if (key == "player_frame_hz")
    {
        char* end = nullptr;
        const long hz = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0')
        {
            if (hz < 10) out.playerFrameHz = 10;
            else if (hz > 60) out.playerFrameHz = 60;
            else out.playerFrameHz = static_cast<int>(hz);
        }
    }
    else if (key == "player_frame_change_only")
        out.playerFrameChangeOnly = AsBool(val);
    else if (key == "npc_motion_update") out.npcMotionUpdate = AsBool(val);
    else if (key == "npc_motion_marker") out.npcMotionMarker = AsBool(val);
    else if (key == "npc_motion_status_p2")
        out.npcMotionStatusP2 = AsBool(val);
    else if (key == "npc_corpse_max_sec")
    {
        char* end = nullptr;
        const long c = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0' && c >= 0 && c <= 600)
            out.npcCorpseMaxSec = static_cast<int>(c);
    }
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
