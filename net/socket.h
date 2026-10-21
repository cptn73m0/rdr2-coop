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
//
// Потокобезопасность: Recv/Send/Flush/Close/IsClosed можно вызывать из разных
// потоков. Внутренний mutex защищает и системный handle (fd_), и очередь —
// закрытие не может произойти между проверкой fd_ и вызовом send(), поэтому
// «send в переиспользованный handle» невозможен.
//
// Защита от OOM: если получатель не читает и очередь превышает
// kMaxPendingBytes, соединение закрывается (Send вернёт Closed, владелец
// заметит это на Recv и корректно освободит ресурсы). Уже отправленный
// префикс очереди периодически освобождается (compaction), поэтому память
// не растёт от «частично слитых» данных.
class Socket {
public:
    // Максимум байт, которые можно держать в очереди на отправку.
    static constexpr std::size_t kMaxPendingBytes = 512 * 1024;
    // Порог, после которого отправленный префикс очереди освобождается.
    static constexpr std::size_t kCompactThreshold = 64 * 1024;

    explicit Socket(SOCKET fd) : fd_(fd) {}
    virtual ~Socket() { Close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Сырой handle. Использовать только если владелец гарантирует, что
    // параллельного Close/Send нет (в текущем коде не используется).
    SOCKET Raw() const;

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

    // Разрыв соединения. Потокобезопасно, идемпотентно; очередь освобождается.
    void Close();

    bool IsClosed() const;

    // Остались ли неотправленные байты в очереди. Нужно для best-effort
    // flush перед Close: неблокирующий Send мог вернуть WouldBlock, а
    // Close освобождает очередь — без этого последнее сообщение теряется.
    bool HasPending() const;

private:
    Result FlushLocked();
    void CloseLocked();
    void CompactLocked();

    mutable std::mutex mtx_;
    SOCKET fd_;
    std::vector<uint8_t> pending_;
    std::size_t pendingOff_ = 0;
};

// Инициализация стека (для Winsock WSAStartup). Вызывать один раз.
bool Init();
void Shutdown();

} // namespace netio
