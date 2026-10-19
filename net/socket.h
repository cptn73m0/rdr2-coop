#pragma once

// Тонкая кроссплатформенная обёртка поверх системных сокетов
// (Winsock на Windows, POSIX на Linux). Цель — один код для сервера и
// клиента, который потом без переписывания соберётся под Linux.
// Сейчас собирается и проверяется Windows-вариант.

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <netdb.h>
#  include <fcntl.h>
typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket(s) ::close(s)
#endif

namespace netio {

enum class Result { Ok, WouldBlock, Closed, Error, NoData };

// Отключить алгоритм Нагла: иначе мелкие кадры (96 байт при 60 Гц) копятся
// до ~40 мс + delayed ACK — это главный источник лага.
bool SetNoDelay(SOCKET fd);

// Неблокирующий режим сокета (иначе send() может зависнуть на медленном
// клиенте и затормозить ретрансляцию остальным).
bool SetNonBlocking(SOCKET fd);

// Сокет с очередью на отправку: Send() никогда не блокирует поток — ставит
// данные в очередь и пытается вылить их сразу. Хвост дошлётся при Flush().
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

    // Запись len байт из buf. Никогда не блокирует: ставит в очередь и
    // пытается отправить немедленно (WouldBlock = остаток в очереди).
    Result Send(const uint8_t* buf, int len);

    // Дослать накопленный хвост (вызывать в цикле чтения).
    void Flush();

    // Разрыв соединения.
    void Close() { if (fd_ != INVALID_SOCKET) { closesocket(fd_); fd_ = INVALID_SOCKET; } }

private:
    Result FlushLocked();

    SOCKET fd_;
    std::mutex sendMtx_;
    std::vector<uint8_t> pending_;
    std::size_t pendingOff_ = 0;
};

// Инициализация стека (для Winsock WSAStartup). Вызывать один раз.
bool Init();
void Shutdown();

} // namespace netio
