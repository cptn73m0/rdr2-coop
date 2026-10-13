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
};

// Загрузить конфиг из rdr2-coop.ini (рядом с плагином). Если файла нет —
// создать с настройками по умолчанию; при ошибках — оставить дефолты.
Config& Load();

// Текущий конфиг (после Load()).
Config& Get();

} // namespace coopcfg