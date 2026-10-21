#include "socket.h"

#include <cerrno>

namespace netio {

namespace {

bool WouldBlockError(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

int LastSocketError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

} // namespace

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

SOCKET Socket::Raw() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return fd_;
}

Result Socket::Recv(uint8_t* buf, int len, int& dataLen)
{
    dataLen = 0;
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ == INVALID_SOCKET) return Result::Closed;
#ifdef _WIN32
    int r = recv(fd_, reinterpret_cast<char*>(buf), len, 0);
    if (r > 0) { dataLen = r; return Result::Ok; }
    if (r == 0) return Result::NoData; // peer closed
    if (WouldBlockError(WSAGetLastError())) return Result::WouldBlock;
    return Result::Error;
#else
    ssize_t r = ::recv(fd_, reinterpret_cast<char*>(buf), len, 0);
    if (r > 0) { dataLen = static_cast<int>(r); return Result::Ok; }
    if (r == 0) return Result::NoData;
    if (WouldBlockError(errno)) return Result::WouldBlock;
    return Result::Error;
#endif
}

void Socket::CompactLocked()
{
    if (pendingOff_ == 0)
        return;
    if (pendingOff_ >= pending_.size())
    {
        pending_.clear();
        pendingOff_ = 0;
        return;
    }
    if (pendingOff_ >= kCompactThreshold)
    {
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<std::ptrdiff_t>(pendingOff_));
        pendingOff_ = 0;
    }
}

Result Socket::FlushLocked()
{
    if (fd_ == INVALID_SOCKET) return Result::Closed;
    for (;;)
    {
        if (pendingOff_ >= pending_.size())
        {
            pending_.clear();
            pendingOff_ = 0;
            return Result::Ok;
        }

        const char* p = reinterpret_cast<const char*>(pending_.data()) + pendingOff_;
        const int left = static_cast<int>(pending_.size() - pendingOff_);
#ifdef _WIN32
        const int r = send(fd_, p, left, 0);
#else
        const ssize_t r = ::send(fd_, p, left, 0);
#endif
        if (r > 0)
        {
            pendingOff_ += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && WouldBlockError(LastSocketError()))
        {
            // Часть данных уже ушла — освобождаем этот префикс, чтобы
            // память не росла у медленно читающего получателя.
            CompactLocked();
            return Result::WouldBlock;
        }
        // r == 0 или реальная ошибка: соединение считается битым. Закрываем,
        // чтобы владелец гарантированно увидел Closed и почистил состояние.
        CloseLocked();
        return Result::Error;
    }
}

Result Socket::Send(const uint8_t* buf, int len)
{
    if (len <= 0) return Result::Ok;
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ == INVALID_SOCKET) return Result::Closed;

    const std::size_t queued = pending_.size() - pendingOff_;
    const std::size_t add = static_cast<std::size_t>(len);
    if (queued + add > kMaxPendingBytes)
    {
        // Получатель не читает: не копим память, рвём соединение. Владелец
        // заметит Closed на Recv и удалит игрока/уйдёт в reconnect.
        CloseLocked();
        return Result::Closed;
    }

    pending_.insert(pending_.end(), buf, buf + len);
    return FlushLocked();
}

void Socket::Flush()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (fd_ == INVALID_SOCKET) return;
    FlushLocked();
}

void Socket::CloseLocked()
{
    if (fd_ != INVALID_SOCKET)
    {
        closesocket(fd_);
        fd_ = INVALID_SOCKET;
    }
    // Мгновенно освобождаем возможные сотни КБ очереди.
    std::vector<uint8_t>().swap(pending_);
    pendingOff_ = 0;
}

void Socket::Close()
{
    std::lock_guard<std::mutex> lk(mtx_);
    CloseLocked();
}

bool Socket::IsClosed() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return fd_ == INVALID_SOCKET;
}

bool Socket::HasPending() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return pendingOff_ < pending_.size();
}

} // namespace netio
