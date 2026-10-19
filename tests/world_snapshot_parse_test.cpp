// Headless stress test for coop::world_sync::ParseWorldSnapshot.
#include "world_snapshot_parse.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

std::vector<uint8_t> MakeSnapshot(const std::vector<net::WorldEntity>& entities)
{
    net::WorldSnapshotHeader header{};
    header.count = static_cast<uint16_t>(entities.size());
    std::vector<uint8_t> payload(sizeof(header) +
        entities.size() * sizeof(net::WorldEntity));
    std::memcpy(payload.data(), &header, sizeof(header));
    for (size_t i = 0; i < entities.size(); ++i)
        std::memcpy(payload.data() + sizeof(header) + i * sizeof(net::WorldEntity),
                    &entities[i], sizeof(net::WorldEntity));
    return payload;
}

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void Worker(unsigned seed, std::atomic<int>& failures)
{
    std::mt19937 rng(seed);
    for (int trial = 0; trial < 20000; ++trial)
    {
        // 1) Valid snapshot of random size, exact round-trip.
        const int count = static_cast<int>(rng() % (net::kMaxWorldEntities + 1));
        std::vector<net::WorldEntity> ents(count);
        for (auto& e : ents)
        {
            e.netId = rng();
            e.model = rng();
            e.x = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) * 0.1f;
            e.y = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) * 0.1f;
            e.z = static_cast<float>(rng() % 100);
            e.heading = static_cast<float>(rng() % 360);
            e.kind = static_cast<uint8_t>(rng() % 4);
            e.flags = static_cast<uint8_t>(rng());
            e.state = static_cast<uint8_t>(rng() % 3);
        }
        auto payload = MakeSnapshot(ents);
        coop::world_sync::ParsedWorldSnapshot parsed;
        if (!coop::world_sync::ParseWorldSnapshot(payload, parsed) ||
            parsed.count != count || parsed.entities.size() != ents.size())
        {
            ++failures;
            return;
        }
        for (int i = 0; i < count; ++i)
        {
            if (std::memcmp(&parsed.entities[i], &ents[i],
                            sizeof(net::WorldEntity)) != 0)
            {
                ++failures;
                return;
            }
        }

        // 2) Random garbage: parser must reject safely, never crash.
        const size_t len = static_cast<size_t>(rng() % 2200);
        std::vector<uint8_t> garbage(len);
        for (auto& b : garbage) b = static_cast<uint8_t>(rng());
        coop::world_sync::ParsedWorldSnapshot ignored;
        (void)coop::world_sync::ParseWorldSnapshot(garbage, ignored);
    }
}

} // namespace

int main()
{
    // Deterministic edge cases.
    {
        coop::world_sync::ParsedWorldSnapshot p;
        Check(!coop::world_sync::ParseWorldSnapshot({}, p), "empty rejected");

        std::vector<uint8_t> shortHeader{1, 0, 0}; // 3 bytes
        Check(!coop::world_sync::ParseWorldSnapshot(shortHeader, p),
              "short header rejected");

        auto zero = MakeSnapshot({});
        Check(coop::world_sync::ParseWorldSnapshot(zero, p) && p.count == 0 &&
              p.entities.empty(), "zero entities accepted");

        auto zeroWithTail = zero;
        zeroWithTail.push_back(0);
        Check(!coop::world_sync::ParseWorldSnapshot(zeroWithTail, p),
              "trailing byte rejected");

        // count=65 (> kMaxWorldEntities=64)
        std::vector<uint8_t> tooMany(4 + 65 * sizeof(net::WorldEntity), 0);
        tooMany[0] = 65;
        Check(!coop::world_sync::ParseWorldSnapshot(tooMany, p),
              "count > max rejected");

        // count says 3, payload for 1.
        std::vector<net::WorldEntity> one(1);
        auto truncated = MakeSnapshot(one);
        truncated[0] = 3;
        Check(!coop::world_sync::ParseWorldSnapshot(truncated, p),
              "truncated payload rejected");
    }

    // Multithreaded stress: 4 x 20k valid+garbage.
    {
        std::atomic<int> threadFailures{0};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i)
            threads.emplace_back(Worker, 500u + i, std::ref(threadFailures));
        for (auto& t : threads) t.join();
        g_failures += threadFailures.load();
        std::printf("world_snapshot_parse_test: multithreaded stress pass\n");
    }

    if (g_failures == 0)
    {
        std::printf("world_snapshot_parse_test: PASS\n");
        return 0;
    }
    std::printf("world_snapshot_parse_test: %d failure(s)\n", g_failures);
    return 1;
}
