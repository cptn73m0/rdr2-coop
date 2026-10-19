// Headless fuzz/robustness test for netio::FrameReader.
// No sockets: checks fragmentation, multiple frames, garbage and oversized
// length headers, and that Poll() never loops forever.
#include "net/frame.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
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

namespace {

// Long-running randomized worker: each thread owns its FrameReader.
void LongRandomWorker(unsigned seed, std::atomic<int>& failures)
{
    std::mt19937 rng(seed);
    for (int trial = 0; trial < 20000; ++trial)
    {
        netio::FrameReader reader;
        std::vector<uint8_t> bytes;
        const bool validFrame = (rng() % 2) != 0;
        if (validFrame)
        {
            const uint8_t type = static_cast<uint8_t>(rng());
            const size_t len = static_cast<size_t>(rng() % 256);
            std::vector<uint8_t> payload(len);
            for (auto& b : payload) b = static_cast<uint8_t>(rng());
            bytes = netio::MakeFrame(type, payload.data(),
                                     static_cast<uint32_t>(payload.size()));
        }
        else
        {
            const size_t len = static_cast<size_t>(rng() % 512);
            bytes.resize(len);
            for (auto& b : bytes) b = static_cast<uint8_t>(rng());
        }

        size_t offset = 0;
        int guard = 0;
        while (offset < bytes.size())
        {
            const size_t chunk = 1 + (rng() % 13);
            const size_t remaining = bytes.size() - offset;
            const size_t take = chunk < remaining ? chunk : remaining;
            reader.Feed(bytes.data() + offset, static_cast<int>(take));
            offset += take;

            uint8_t t = 0;
            std::vector<uint8_t> p;
            while (reader.Poll(t, p))
            {
                if (++guard > 10000)
                {
                    ++failures;
                    return;
                }
            }
        }
    }
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

    // 6) Byte-by-byte feeding of a long valid stream must preserve order.
    {
        netio::FrameReader reader;
        std::vector<std::vector<uint8_t>> payloads;
        std::vector<uint8_t> stream;
        std::mt19937 rng(0xBEEF);
        for (int i = 0; i < 500; ++i)
        {
            std::vector<uint8_t> payload(static_cast<size_t>(rng() % 40));
            for (auto& b : payload) b = static_cast<uint8_t>(rng());
            payloads.push_back(payload);
            auto f = netio::MakeFrame(static_cast<uint8_t>(0x20 + (i % 0x20)),
                                      payload.data(),
                                      static_cast<uint32_t>(payload.size()));
            stream.insert(stream.end(), f.begin(), f.end());
        }
        size_t got = 0;
        size_t offset = 0;
        uint8_t t = 0;
        std::vector<uint8_t> p;
        while (offset < stream.size())
        {
            reader.Feed(stream.data() + offset, 1);
            ++offset;
            while (reader.Poll(t, p))
            {
                if (got < payloads.size() && p == payloads[got])
                    ++got;
                else
                {
                    std::printf("FAIL: byte-stream payload mismatch at %zu\n", got);
                    ++failures;
                    break;
                }
            }
        }
        if (got != payloads.size())
        {
            std::printf("FAIL: byte-stream frames got=%zu expected=%zu\n",
                        got, payloads.size());
            ++failures;
        }
    }

    // 7) Long-running multithreaded random fuzz (4 threads x 20k).
    {
        std::atomic<int> threadFailures{0};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i)
            threads.emplace_back(LongRandomWorker, 1000u + i,
                                 std::ref(threadFailures));
        for (auto& th : threads) th.join();
        failures += threadFailures.load();
        std::printf("frame_reader_fuzz: long multithreaded pass\n");
    }

    if (failures == 0)
    {
        std::printf("frame_reader_fuzz: PASS\n");
        return 0;
    }
    std::printf("frame_reader_fuzz: %d failure(s)\n", failures);
    return 1;
}
