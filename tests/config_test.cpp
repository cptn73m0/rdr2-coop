// Headless tests for coopcfg::LoadFromFile (rdr2-coop.ini parser).
#include "config.h"

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
    return std::filesystem::temp_directory_path() / "rdr2coop-config-test.ini";
}

void Write(const std::string& text)
{
    std::ofstream f(TempIni(), std::ios::trunc);
    f << text;
}

} // namespace

int main()
{
    const auto path = TempIni();

    // 1) Full parse, comments/whitespace/unknown keys, hex model.
    Write(
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
        "world_mission_entities = 1\n"
        "world_suppress_ambient = 1\n"
        "vehicle_crew = 1\n"
        "world_debug = 1\n"
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
        Check(cfg.worldMissionEntities, "world_mission_entities");
        Check(cfg.worldSuppressAmbient, "world_suppress_ambient");
        Check(cfg.vehicleCrew, "vehicle_crew");
        Check(cfg.worldDebug, "world_debug");
    }

    // 2) False variants and decimal model.
    Write(
        "world_sync = false\n"
        "action_sync = no\n"
        "vehicle_sync = OFF\n"
        "mission_sync = 0\n"
        "mission_credit = off\n"
        "save_backup = False\n"
        "mission_reward_log = 0\n"
        "world_trains = false\n"
        "train_debug = false\n"
        "world_mission_entities = false\n"
        "world_suppress_ambient = false\n"
        "vehicle_crew = false\n"
        "world_debug = false\n"
        "ped_model = 123456\n");
    {
        coopcfg::Config cfg;
        Check(coopcfg::LoadFromFile(path.string(), cfg), "false file should load");
        Check(!cfg.worldSync && !cfg.actionSync && !cfg.vehicleSync &&
              !cfg.missionSync && !cfg.missionCredit && !cfg.saveBackup &&
              !cfg.missionRewardLog && !cfg.worldTrains && !cfg.trainDebug &&
              !cfg.worldMissionEntities && !cfg.worldSuppressAmbient &&
              !cfg.vehicleCrew && !cfg.worldDebug,
              "all false variants");
        Check(cfg.pedModel == 123456ull, "ped_model decimal");
    }

    // 3) ped_model=0 must not overwrite the default; empty values ignored.
    {
        coopcfg::Config cfg;
        const auto defaultModel = cfg.pedModel;
        Write("ped_model = 0\nserver_host =\nplayer_name = X\n");
        Check(coopcfg::LoadFromFile(path.string(), cfg), "zero model file loads");
        Check(cfg.pedModel == defaultModel, "ped_model=0 keeps default");
        Check(cfg.playerName == "X", "empty value ignored");
    }

    // 4) Missing file fails; defaults untouched.
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
