#include "socket.h"

#include <cerrno>

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

bool SetNoDelay(SOCKET fd)
{
    if (fd == INVALID_SOCKET) return false;
    int one = 1;
#ifdef _WIN32
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
#else
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
#endif
}

bool SetNonBlocking(SOCKET fd)
{
    if (fd == INVALID_SOCKET) return false;
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
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

Result Socket::FlushLocked()
{
    if (fd_ == INVALID_SOCKET) return Result::Closed;
    while (pendingOff_ < pending_.size())
    {
        const char* p = reinterpret_cast<const char*>(pending_.data()) + pendingOff_;
        int left = static_cast<int>(pending_.size() - pendingOff_);
#ifdef _WIN32
        int r = send(fd_, p, left, 0);
        if (r > 0) { pendingOff_ += static_cast<std::size_t>(r); continue; }
        if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return Result::WouldBlock;
#else
        ssize_t r = ::send(fd_, p, left, 0);
        if (r > 0) { pendingOff_ += static_cast<std::size_t>(r); continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Result::WouldBlock;
#endif
        return Result::Error;
    }
    pending_.clear();
    pendingOff_ = 0;
    return Result::Ok;
}

Result Socket::Send(const uint8_t* buf, int len)
{
    if (fd_ == INVALID_SOCKET) return Result::Closed;
    if (len <= 0) return Result::Ok;
    // Не блокируем вызывающий поток: кладём в очередь и пробуем отправить.
    std::lock_guard<std::mutex> lk(sendMtx_);
    pending_.insert(pending_.end(), buf, buf + len);
    return FlushLocked();
}

void Socket::Flush()
{
    if (fd_ == INVALID_SOCKET) return;
    std::lock_guard<std::mutex> lk(sendMtx_);
    FlushLocked();
}

} // namespace netio
