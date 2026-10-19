// Headless fuzz/robustness test for netio::FrameReader.
// No sockets: checks fragmentation, multiple frames, garbage and oversized
// length headers, and that Poll() never loops forever.
#include "net/frame.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

bool FeedAndPoll(netio::FrameReader& reader, const std::vector<uint8_t>& bytes,
                 uint8_t& type, std::vector<uint8_t>& payload)
{
    size_t offset = 0;
    bool got = false;
    while (offset < bytes.size())
    {
        const size_t chunk = (bytes.size() - offset > 7) ? 7 : (bytes.size() - offset);
        reader.Feed(bytes.data() + offset, static_cast<int>(chunk));
        offset += chunk;
        uint8_t t = 0;
        std::vector<uint8_t> p;
        while (reader.Poll(t, p))
        {
            type = t;
            payload = p;
            got = true;
        }
    }
    return got;
}

} // namespace

int main()
{
    std::mt19937 rng(0xC0FFEE);
    int failures = 0;

    // 1) Valid frames, random chunking, multiple frames back-to-back.
    for (int trial = 0; trial < 200; ++trial)
    {
        const uint8_t type = static_cast<uint8_t>(0x10 + (trial % 10));
        const size_t len = static_cast<size_t>(rng() % 300);
        std::vector<uint8_t> payload(len);
        for (auto& b : payload) b = static_cast<uint8_t>(rng());
        auto frame = netio::MakeFrame(type, payload.data(),
                                      static_cast<uint32_t>(payload.size()));

        netio::FrameReader reader;
        uint8_t gotType = 0;
        std::vector<uint8_t> gotPayload;
        if (!FeedAndPoll(reader, frame, gotType, gotPayload) ||
            gotType != type || gotPayload != payload)
        {
            std::printf("FAIL: valid frame mismatch trial=%d\n", trial);
            ++failures;
        }
    }

    // 2) Two frames fed as one buffer.
    {
        netio::FrameReader reader;
        std::vector<uint8_t> p1{1, 2, 3};
        std::vector<uint8_t> p2{4, 5, 6, 7};
        auto f1 = netio::MakeFrame(0x11, p1.data(), (uint32_t)p1.size());
        auto f2 = netio::MakeFrame(0x12, p2.data(), (uint32_t)p2.size());
        f1.insert(f1.end(), f2.begin(), f2.end());
        reader.Feed(f1.data(), (int)f1.size());
        uint8_t t = 0;
        std::vector<uint8_t> p;
        if (!reader.Poll(t, p) || t != 0x11 || p != p1)
        {
            std::printf("FAIL: first frame of pair\n");
            ++failures;
        }
        if (!reader.Poll(t, p) || t != 0x12 || p != p2)
        {
            std::printf("FAIL: second frame of pair\n");
            ++failures;
        }
        if (reader.Poll(t, p))
        {
            std::printf("FAIL: extra frame after pair\n");
            ++failures;
        }
    }

    // 3) Random garbage: must not hang or crash.
    for (int trial = 0; trial < 300; ++trial)
    {
        netio::FrameReader reader;
        const int len = static_cast<int>(rng() % 400);
        std::vector<uint8_t> garbage(len);
        for (auto& b : garbage) b = static_cast<uint8_t>(rng());
        reader.Feed(garbage.data(), len);
        uint8_t t = 0;
        std::vector<uint8_t> p;
        int guard = 0;
        while (reader.Poll(t, p))
        {
            if (++guard > 10000)
            {
                std::printf("FAIL: Poll loop did not terminate\n");
                ++failures;
                break;
            }
        }
    }

    // 4) Oversized declared length (> kMaxPayload) must reset, not allocate.
    {
        netio::FrameReader reader;
        std::vector<uint8_t> bad{0x00, 0x00, 0x02, 0x00, 0x10}; // 0x20000
        reader.Feed(bad.data(), (int)bad.size());
        uint8_t t = 0;
        std::vector<uint8_t> p;
        if (reader.Poll(t, p))
        {
            std::printf("FAIL: oversized length produced frame\n");
            ++failures;
        }
        // Reader recovers on next valid frame.
        std::vector<uint8_t> payload{9, 8, 7};
        auto frame = netio::MakeFrame(0x13, payload.data(), (uint32_t)payload.size());
        reader.Feed(frame.data(), (int)frame.size());
        if (!reader.Poll(t, p) || t != 0x13 || p != payload)
        {
            std::printf("FAIL: reader did not recover after oversized length\n");
            ++failures;
        }
    }

    // 5) Truncated frame: no output, no hang.
    {
        netio::FrameReader reader;
        std::vector<uint8_t> payload(50, 0xAB);
        auto frame = netio::MakeFrame(0x14, payload.data(), (uint32_t)payload.size());
        reader.Feed(frame.data(), (int)frame.size() - 10);
        uint8_t t = 0;
        std::vector<uint8_t> p;
        if (reader.Poll(t, p))
        {
            std::printf("FAIL: truncated frame produced output\n");
            ++failures;
        }
    }

    if (failures == 0)
    {
        std::printf("frame_reader_fuzz: PASS\n");
        return 0;
    }
    std::printf("frame_reader_fuzz: %d failure(s)\n", failures);
    return 1;
}
