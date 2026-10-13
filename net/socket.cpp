#include "socket.h"

namespace netio {

bool Init()
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void Shutdown()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

Result Socket::Recv(uint8_t* buf, int len, int& dataLen)
{
    dataLen = 0;
    if (fd_ == INVALID_SOCKET) return Result::Closed;
#ifdef _WIN32
    int r = recv(fd_, reinterpret_cast<char*>(buf), len, 0);
    if (r > 0) { dataLen = r; return Result::Ok; }
    if (r == 0) return Result::NoData; // peer closed
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return Result::WouldBlock;
    return Result::Error;
#else
    ssize_t r = ::recv(fd_, reinterpret_cast<char*>(buf), len, 0);
    if (r > 0) { dataLen = static_cast<int>(r); return Result::Ok; }
    if (r == 0) return Result::NoData;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Result::WouldBlock;
    return Result::Error;
#endif
}

Result Socket::Send(const uint8_t* buf, int len)
{
    if (fd_ == INVALID_SOCKET) return Result::Closed;
#ifdef _WIN32
    int r = send(fd_, reinterpret_cast<const char*>(buf), len, 0);
    if (r > 0) return r == len ? Result::Ok : Result::WouldBlock; // partial
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return Result::WouldBlock;
    return Result::Error;
#else
    ssize_t r = ::send(fd_, reinterpret_cast<const char*>(buf), len, 0);
    if (r > 0) return r == len ? Result::Ok : Result::WouldBlock;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Result::WouldBlock;
    return Result::Error;
#endif
}

} // namespace netio