#pragma once

// Тонкая кроссплатформенная обёртка поверх системных сокетов
// (Winsock на Windows, POSIX на Linux). Цель — один код для сервера и
// клиента, который потом без переписывания соберётся под Linux.
// Сейчас собирается и проверяется Windows-вариант.

#include <cstdint>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <netdb.h>
typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket(s) ::close(s)
#endif

namespace netio {

enum class Result { Ok, WouldBlock, Closed, Error, NoData };

// Простой клиентский и серверный сокет в неблокирующем режиме.
class Socket {
public:
    explicit Socket(SOCKET fd) : fd_(fd) {}
    virtual ~Socket() { if (fd_ != INVALID_SOCKET) closesocket(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET Raw() const { return fd_; }

    // Чтение до len байт в buf. Возвращает:
    //   Ok          — прочитано dataLen байт (>0)
    //   WouldBlock  — данных пока нет
    //   NoData      — соединение закрыто/сброшено
    Result Recv(uint8_t* buf, int len, int& dataLen);

    // Запись len байт из buf (полная передача или WouldBlock).
    Result Send(const uint8_t* buf, int len);

    // Разрыв соединения.
    void Close() { if (fd_ != INVALID_SOCKET) { closesocket(fd_); fd_ = INVALID_SOCKET; } }

private:
    SOCKET fd_;
};

// Инициализация стека (для Winsock WSAStartup). Вызывать один раз.
bool Init();
void Shutdown();

} // namespace netio