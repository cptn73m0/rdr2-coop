#pragma once

// Пользовательская конфигурация плагина: адрес/порт целевого сервера,
// ник игрока и модель персонажа. Читается из rdr2-coop.ini рядом с .asi.
//
// Пример rdr2-coop.ini:
//   server_host = 127.0.0.1
//   server_port = 7777
//   player_name = Arthur
//   ped_model   = 0xCF7E73BE

#include <string>

namespace coopcfg {

struct Config {
    std::string serverHost = "127.0.0.1";
    int         serverPort = 7777;
    std::string playerName = "Player";
    unsigned long long pedModel = 0xCF7E73BEull; // a_m_m_armtownfolk_01
    // P1: репликация мира/NPC (хост -> гость). По умолчанию выключена,
    // пока функция не проверена на двух ПК.
    bool worldSync = false;
    // P2: Action v2 — сценарии/сидение/курение/укрытие и др. действий.
    // По умолчанию выключена до теста на двух ПК.
    bool actionSync = false;
    // P2: посадка в транспорт (дилижанс/повозка) удалённого игрока.
    bool vehicleSync = false;
    // P3: хост детектит новые завершённые миссии MissionData и шлёт гостю.
    bool missionSync = false;
    // P3: писать зачёт миссии в свой сейв гостя (опасно, если прогресс разный).
    bool missionCredit = false;
    // P3: перед первым зачётом сделать бэкап сейва гостя. Если бэкап не
    // удался — зачёт не пишется (защита от порчи прогресса).
    bool saveBackup = true;
    // P3: логировать план наград миссии (dry-run, нативы не вызываются).
    bool missionRewardLog = true;
    // R&D: экспериментальная репликация поездов/состава (хост -> гость).
    // По умолчанию выключено: движущийся состав требует отдельной модели.
    bool worldTrains = false;
    // R&D: подробный лог offset/attach для TrainSnapshot (диагностика).
    bool trainDebug = false;
    // P1 safe mode: по умолчанию НЕ реплицируем mission-peds/objects — они
    // без миссионного контекста ведут себя как свободные прокси (летают и т.п.).
    bool worldMissionEntities = false;
    // P1 safe mode: глушить только ambient людей на госте. Животные и
    // транспорт НЕ глушатся, иначе пропадают лошади/тягловые.
    bool worldSuppressAmbient = false;
    // Crew: синхронизировать место водителя/пассажиров в транспорте.
    bool vehicleCrew = false;
    // Диагностика world_sync: логировать позиции прокси (отладка телепортов).
    bool worldDebug = false;
};

// Загрузить конфиг из rdr2-coop.ini (рядом с плагином). Если файла нет —
// создать с настройками по умолчанию; при ошибках — оставить дефолты.
Config& Load();

// Разобрать INI из произвольного пути (для тестов/диагностики).
// Возвращает false, если файл не открылся.
bool LoadFromFile(const std::string& path, Config& out);

// Текущий конфиг (после Load()).
Config& Get();

} // namespace coopcfg