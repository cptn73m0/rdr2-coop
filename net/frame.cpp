#include "frame.h"

#include <cstring>

namespace netio {

void FrameReader::Feed(const uint8_t* data, int len)
{
    if (len > 0)
        buf_.insert(buf_.end(), data, data + len);
    if (buf_.size() > net::kMaxPayload + net::kHeaderSize)
        Reset(); // защита от runaway buffer
}

bool FrameReader::Poll(uint8_t& outType, std::vector<uint8_t>& outPayload)
{
    if (buf_.size() < net::kHeaderSize)
        return false;

    // len (LE u32)
    uint32_t len =
        static_cast<uint32_t>(buf_[0]) |
        (static_cast<uint32_t>(buf_[1]) << 8) |
        (static_cast<uint32_t>(buf_[2]) << 16) |
        (static_cast<uint32_t>(buf_[3]) << 24);

    if (len > net::kMaxPayload)
    {
        Reset();
        return false;
    }

    // type
    uint8_t type = buf_[net::kLenSize];

    std::size_t total = net::kHeaderSize + len;
    if (buf_.size() < total)
        return false; // ещё не весь payload

    outType = type;
    outPayload.assign(buf_.begin() + net::kHeaderSize, buf_.begin() + total);
    buf_.erase(buf_.begin(), buf_.begin() + total);
    return true;
}

void FrameReader::Reset()
{
    buf_.clear();
}

std::vector<uint8_t> MakeFrame(uint8_t type, const uint8_t* payload, uint32_t len)
{
    uint8_t hdr[net::kHeaderSize];
    hdr[0] = static_cast<uint8_t>(len & 0xFF);
    hdr[1] = static_cast<uint8_t>((len >> 8) & 0xFF);
    hdr[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
    hdr[3] = static_cast<uint8_t>((len >> 24) & 0xFF);
    hdr[4] = type;

    std::vector<uint8_t> frame;
    frame.reserve(net::kHeaderSize + len);
    frame.insert(frame.end(), hdr, hdr + net::kHeaderSize);
    if (payload && len)
        frame.insert(frame.end(), payload, payload + len);
    return frame;
}

Result SendFrame(Socket& s, uint8_t type, const uint8_t* payload, uint32_t len)
{
    std::vector<uint8_t> frame = MakeFrame(type, payload, len);
    return s.Send(frame.data(), static_cast<int>(frame.size()));
}

} // namespace netio