#pragma once

// Разбор и сборка кадров протокола поверх net::Socket.
// Кадр: [len:u32][type:u8][payload...]. Чтение по частям (неблокирующее).

#include <cstdint>
#include <vector>

#include "net/protocol.h"
#include "net/socket.h"

namespace netio {

// Накапливает входящие байты и выдаёт целые сообщения.
class FrameReader {
public:
    // Скормить прочитанные данные. Возвращает Ok если данных принято,
    // WouldBlock если некритично.
    void Feed(const uint8_t* data, int len);

    // Выложить следующее целое сообщение (type + payload) если есть.
    // Возвращает true и заполняет outType/outPayload.
    bool Poll(uint8_t& outType, std::vector<uint8_t>& outPayload);

    void Reset();

private:
    std::vector<uint8_t> buf_;
};

// Собрать кадр из type+payload в один буфер и отправить.
// Возвращает Result.
Result SendFrame(Socket& s, uint8_t type, const uint8_t* payload, uint32_t len);

// Собрать кадр (type + payload) в вектор — без отправки. Используется
// и клиентом, и сервером для построения исходящих сообщений.
std::vector<uint8_t> MakeFrame(uint8_t type, const uint8_t* payload, uint32_t len);

} // namespace netio