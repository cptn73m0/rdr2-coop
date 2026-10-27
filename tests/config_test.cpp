// Headless tests for coopcfg::LoadFromFile (rdr2-coop.ini parser).
//
// Tests encode the P0 config-parser behaviour in src/config.cpp:
//   * boolean values are case-insensitive ("FALSE"/"No"/"Off" => false);
//   * invalid server_port (0/70000/"abc") does not silently override the default;
//   * inline `; comment` after a value is stripped (README example must parse);
//   * unknown keys are ignored and duplicate keys use the last value.
//
// All checks are strict (Check -> FAIL): a parser regression must fail the test,
// not be reported as a non-fatal "pending".
#include "config.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::filesystem::path TempIni()
{
    // Unique per process/run: two test runs must not share one temp file.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("rdr2coop-config-test-" + std::to_string(static_cast<long long>(stamp)) + ".ini");
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream f(path, std::ios::trunc);
    f << text;
}

} // namespace

int main()
{
    const auto path = TempIni();

    // 0) Documented defaults (config.h / README flag table) must match code.
    {
        coopcfg::Config cfg;
        Check(cfg.serverHost == "127.0.0.1", "default server_host");
        Check(cfg.serverPort == 7777, "default server_port");
        Check(cfg.playerName == "Player", "default player_name");
        Check(cfg.pedModel == 0xCF7E73BEull, "default ped_model");
        Check(!cfg.worldSync && !cfg.actionSync && !cfg.vehicleSync &&
              !cfg.missionSync && !cfg.missionCredit,
              "defaults: experimental P1/P2/P3 flags off");
        Check(cfg.saveBackup, "default save_backup = 1");
        Check(cfg.missionRewardLog, "default mission_reward_log = 1");
        Check(!cfg.worldTrains && !cfg.trainDebug, "defaults: R&D trains off");
    }

    // 1) Full parse, comments/whitespace/unknown keys, hex model.
    Write(path,
        "# comment\n"
        "; another comment\n"
        "  server_host = 10.0.0.5  \r\n"
        "server_port=12345\n"
        "player_name =  Test User \n"
        "ped_model = 0xABCDEF01\n"
        "world_sync = 1\n"
        "action_sync = true\n"
        "vehicle_sync = yes\n"
        "mission_sync = on\n"
        "mission_credit = 1\n"
        "save_backup = 0\n"
        "mission_reward_log = off\n"
        "world_trains = 1\n"
        "train_debug = 1\n"
        "unknown_key = whatever\n"
        "no_equals_here\n");
    {
        coopcfg::Config cfg;
        Check(coopcfg::LoadFromFile(path.string(), cfg), "file should load");
        Check(cfg.serverHost == "10.0.0.5", "server_host");
        Check(cfg.serverPort == 12345, "server_port");
        Check(cfg.playerName == "Test User", "player_name trim");
        Check(cfg.pedModel == 0xABCDEF01ull, "ped_model hex");
        Check(cfg.worldSync, "world_sync");
        Check(cfg.actionSync, "action_sync");
        Check(cfg.vehicleSync, "vehicle_sync");
        Check(cfg.missionSync, "mission_sync");
        Check(cfg.missionCredit, "mission_credit");
        Check(!cfg.saveBackup, "save_backup off");
        Check(!cfg.missionRewardLog, "mission_reward_log off");
        Check(cfg.worldTrains, "world_trains");
        Check(cfg.trainDebug, "train_debug");
    }

    // 2) False variants and decimal model (documented lowercase forms).
    Write(path,
        "world_sync = false\n"
        "action_sync = no\n"
        "vehicle_sync = OFF\n"
        "mission_sync = 0\n"
        "mission_credit = off\n"
        "save_backup = False\n"
        "mission_reward_log = 0\n"
        "world_trains = false\n"
        "train_debug = false\n"
        "ped_model = 123456\n");
    {
        coopcfg::Config cfg;
        Check(coopcfg::LoadFromFile(path.string(), cfg), "false file should load");
        Check(!cfg.worldSync && !cfg.actionSync && !cfg.vehicleSync &&
              !cfg.missionSync && !cfg.missionCredit && !cfg.saveBackup &&
              !cfg.missionRewardLog && !cfg.worldTrains && !cfg.trainDebug,
              "all false variants");
        Check(cfg.pedModel == 123456ull, "ped_model decimal");
    }

    // 3) Case-insensitive booleans (P0 expected; uppercase/mixed forms).
    Write(path,
        "world_sync = FALSE\n"
        "action_sync = No\n"
        "vehicle_sync = Off\n"
        "mission_sync = fAlSe\n"
        "mission_credit = NO\n"
        "save_backup = FALSE\n"
        "mission_reward_log = OFF\n"
        "world_trains = No\n"
        "train_debug = oFf\n");
    {
        coopcfg::Config cfg;
        Check(coopcfg::LoadFromFile(path.string(), cfg), "case-insensitive file loads");
        Check(!cfg.worldSync, "world_sync = FALSE => false");
        Check(!cfg.actionSync, "action_sync = No => false");
        Check(!cfg.vehicleSync, "vehicle_sync = Off => false");
        Check(!cfg.missionSync, "mission_sync = fAlSe => false");
        Check(!cfg.missionCredit, "mission_credit = NO => false");
        Check(!cfg.saveBackup, "save_backup = FALSE => false");
        Check(!cfg.missionRewardLog, "mission_reward_log = OFF => false");
        Check(!cfg.worldTrains, "world_trains = No => false");
        Check(!cfg.trainDebug, "train_debug = oFf => false");
    }

    // 4) Invalid ports must not override the default; valid 65535 accepted.
    {
        coopcfg::Config cfg;
        Write(path, "server_port = 0\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "port=0 file loads");
        Check(cfg.serverPort == 7777, "server_port = 0 keeps default 7777");
    }
    {
        coopcfg::Config cfg;
        Write(path, "server_port = 70000\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "port=70000 file loads");
        Check(cfg.serverPort == 7777, "server_port = 70000 keeps default 7777");
    }
    {
        coopcfg::Config cfg;
        Write(path, "server_port = abc\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "port=abc file loads");
        Check(cfg.serverPort == 7777, "server_port = abc keeps default 7777");
    }
    {
        coopcfg::Config cfg;
        Write(path, "server_port = 65535\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "port=65535 file loads");
        Check(cfg.serverPort == 65535, "server_port = 65535 accepted");
    }

    // 4b) Integer ranges: bounds are enforced; clamp keys clamp, others ignore
    // an out-of-range value and keep the default.
    {
        coopcfg::Config cfg;
        const int defaultCap = cfg.npcCap;
        const int defaultRadius = cfg.npcRadius;
        const int defaultVehicleRadius = cfg.npcVehicleRadius;
        const int defaultAnimalCap = cfg.npcAnimalCap;
        const int defaultFrameHz = cfg.npcFrameHz;
        const int defaultCorpse = cfg.npcCorpseMaxSec;
        Write(path,
            "npc_cap = 0\n"
            "npc_radius = 19\n"
            "npc_vehicle_radius = 201\n"
            "npc_animal_cap = 13\n"
            "npc_frame_hz = 9\n"
            "npc_corpse_max_sec = 601\n"
            "player_frame_hz = 5\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "range file loads");
        Check(cfg.npcCap == defaultCap, "npc_cap below min keeps default");
        Check(cfg.npcRadius == defaultRadius, "npc_radius below min keeps default");
        Check(cfg.npcVehicleRadius == defaultVehicleRadius,
              "npc_vehicle_radius above max keeps default");
        Check(cfg.npcAnimalCap == defaultAnimalCap,
              "npc_animal_cap above max keeps default");
        Check(cfg.npcFrameHz == defaultFrameHz,
              "npc_frame_hz below min keeps default");
        Check(cfg.npcCorpseMaxSec == defaultCorpse,
              "npc_corpse_max_sec above max keeps default");
        Check(cfg.playerFrameHz == 10,
              "player_frame_hz below min clamps to 10");
    }
    {
        coopcfg::Config cfg;
        Write(path,
            "npc_cap = 12\n"
            "npc_radius = 100\n"
            "npc_vehicle_radius = 20\n"
            "npc_animal_cap = 0\n"
            "npc_frame_hz = 20\n"
            "npc_corpse_max_sec = 600\n"
            "player_frame_hz = 61\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "boundary file loads");
        Check(cfg.npcCap == 12, "npc_cap max accepted");
        Check(cfg.npcRadius == 100, "npc_radius max accepted");
        Check(cfg.npcVehicleRadius == 20, "npc_vehicle_radius min accepted");
        Check(cfg.npcAnimalCap == 0, "npc_animal_cap zero accepted");
        Check(cfg.npcFrameHz == 20, "npc_frame_hz max accepted");
        Check(cfg.npcCorpseMaxSec == 600, "npc_corpse_max_sec max accepted");
        Check(cfg.playerFrameHz == 60,
              "player_frame_hz above max clamps to 60");
    }

    // 5) Unknown keys are ignored; duplicate keys: last value wins.
    Write(path,
        "world_mission_entities = 1\n"
        "world_debug = 1\n"
        "world_npc = 1\n"
        "server_host = 10.2.2.2\n"
        "server_port = 1111\n"
        "server_port = 2222\n"
        "player_name = Dup Name\n");
    {
        coopcfg::Config cfg;
        Check(coopcfg::LoadFromFile(path.string(), cfg), "unknown/duplicate file loads");
        Check(cfg.serverHost == "10.2.2.2", "unknown keys do not break following keys");
        Check(cfg.serverPort == 2222, "duplicate server_port: last value wins");
        Check(cfg.playerName == "Dup Name", "duplicate file player_name");
    }

    // 5b) Boolean values and duplicate booleans: only documented false tokens
    // are false; an unknown token is true, and the last duplicate wins.
    {
        coopcfg::Config cfg;
        Write(path,
            "npc_actor = maybe\n"
            "npc_debug = 1\n"
            "npc_debug = 0\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "bool file loads");
        Check(cfg.npcActor, "unknown bool token is truthy");
        Check(!cfg.npcDebug, "duplicate bool: last value wins");
    }

    // 6) README example INI: inline `; comment` must be stripped (P0 expected).
    Write(path,
        "server_host = 192.168.1.103   ; PC with the server\n"
        "server_port = 7777\n"
        "player_name = Player2 ; nick\n"
        "ped_model   = 0xCF7E73BE      ; fallback model\n");
    {
        coopcfg::Config cfg;
        Check(coopcfg::LoadFromFile(path.string(), cfg), "README-style ini loads");
        Check(cfg.serverHost == "192.168.1.103",
              "inline ';' comment stripped from server_host");
        Check(cfg.serverPort == 7777, "README-style server_port");
        Check(cfg.playerName == "Player2",
              "inline ';' comment stripped from player_name");
        Check(cfg.pedModel == 0xCF7E73BEull, "README-style ped_model hex");
    }

    // 6b) Inline comment only when preceded by whitespace: '#' inside a token
    // (e.g. a nick) is preserved, while a spaced comment is stripped.
    {
        coopcfg::Config cfg;
        Write(path,
            "server_host = host#1\n"
            "player_name = Name # comment\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "hash-name file loads");
        Check(cfg.serverHost == "host#1", "embedded '#' preserved");
        Check(cfg.playerName == "Name", "spaced '#' comment stripped");
    }

    // 7) ped_model=0 must not overwrite the default; empty values ignored.
    {
        coopcfg::Config cfg;
        const auto defaultModel = cfg.pedModel;
        Write(path, "ped_model = 0\nserver_host =\nplayer_name = X\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "zero model file loads");
        Check(cfg.pedModel == defaultModel, "ped_model=0 keeps default");
        Check(cfg.playerName == "X", "empty value ignored");
    }

    // 8) Uppercase 0X prefix (same as 0x).
    {
        coopcfg::Config cfg;
        Write(path, "ped_model = 0XABCDEF01\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "0X model file loads");
        Check(cfg.pedModel == 0xABCDEF01ull, "ped_model uppercase 0X hex");
    }

    // 9) Missing file fails; defaults untouched.
    {
        coopcfg::Config cfg;
        cfg.serverHost = "sentinel";
        Check(!coopcfg::LoadFromFile((path.string() + ".missing"), cfg),
              "missing file should fail");
        Check(cfg.serverHost == "sentinel", "defaults untouched on failure");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);

    if (g_failures == 0)
    {
        std::printf("config_test: PASS\n");
        return 0;
    }
    std::printf("config_test: %d failure(s)\n", g_failures);
    return 1;
}
