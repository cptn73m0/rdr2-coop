#pragma once

// Крошечная UDP-обёртка для ретрансляции кадров состояния (Frame) и событий
// выстрела (Shot). Кадр — идемпотентное состояние: потеря одного не критична,
// поэтому UDP без head-of-line blocking даёт меньше пикового лага, чем TCP.
//
// Формат дейтаграммы: [session_id u64 LE][type u8][payload].
// session_id позволяет серверу и клиенту связать дейтаграмму с игроком.

#include <cstdint>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define closesocket(s) ::close(s)
#endif

namespace netio {

// Создать UDP-сокет (SOCK_DGRAM).
bool UdpCreate(SOCKET& fd);
void UdpClose(SOCKET fd);

// Привязать сокет к порту (INADDR_ANY). Для сервера-релея — порт TCP-сервера.
bool UdpBind(SOCKET fd, int port);

// Отправить дейтаграмму в `to`.
bool UdpSendTo(SOCKET fd, const sockaddr_in& to, const void* buf, int len);

// Блокирующий приём дейтаграммы. Возвращает число байт (>0) или -1/0 при
// ошибке (для неблокирующего сокета -1 и errno=WSAEWOULDBLOCK). `from` — адрес
// отправителя.
int UdpRecvFrom(SOCKET fd, void* buf, int len, sockaddr_in& from);

} // namespace netio