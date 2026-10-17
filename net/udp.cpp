#include "udp.h"

#include <cstring>

namespace netio {

bool UdpCreate(SOCKET& fd)
{
#ifdef _WIN32
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
#endif
    return fd != INVALID_SOCKET;
}

void UdpClose(SOCKET fd)
{
    if (fd != INVALID_SOCKET)
        closesocket(fd);
}

bool UdpBind(SOCKET fd, int port)
{
    if (fd == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
#ifdef _WIN32
    return bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#else
    return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#endif
}

bool UdpSendTo(SOCKET fd, const sockaddr_in& to, const void* buf, int len)
{
    if (fd == INVALID_SOCKET) return false;
#ifdef _WIN32
    int r = sendto(fd, reinterpret_cast<const char*>(buf), len, 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#else
    ssize_t r = ::sendto(fd, reinterpret_cast<const char*>(buf), len, 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
    return r == len;
}

int UdpRecvFrom(SOCKET fd, void* buf, int len, sockaddr_in& from)
{
    if (fd == INVALID_SOCKET) return -1;
#ifdef _WIN32
    int fromLen = sizeof(from);
    int r = recvfrom(fd, reinterpret_cast<char*>(buf), len, 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
    socklen_t fromLen = sizeof(from);
    ssize_t r = ::recvfrom(fd, reinterpret_cast<char*>(buf), len, 0,
                           reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif
    return static_cast<int>(r);
}

} // namespace netio