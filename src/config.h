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
};

// Загрузить конфиг из rdr2-coop.ini (рядом с плагином). Если файла нет —
// создать с настройками по умолчанию; при ошибках — оставить дефолты.
Config& Load();

// Текущий конфиг (после Load()).
Config& Get();

} // namespace coopcfg