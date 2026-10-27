// Headless stress test for coop::world_sync::SelectWorldCandidates.
#include "world_entity_budget.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace {

using coop::world_sync::BudgetCandidate;
using coop::world_sync::BudgetCaps;
using coop::world_sync::BudgetKind;

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// Independent reference: stable sort by (mission desc, distance asc), then
// per-kind caps in that order.
std::vector<int> Reference(const std::vector<BudgetCandidate>& in,
                           const BudgetCaps& caps)
{
    std::vector<int> order(in.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& ca = in[a];
        const auto& cb = in[b];
        if (ca.mission != cb.mission) return ca.mission;
        return ca.distance2 < cb.distance2;
    });
    int p = 0, v = 0, o = 0;
    std::vector<int> out;
    for (int i : order)
    {
        switch (in[i].kind)
        {
        case BudgetKind::Ped: if (p >= caps.peds) continue; ++p; break;
        case BudgetKind::Vehicle: if (v >= caps.vehicles) continue; ++v; break;
        case BudgetKind::Object: if (o >= caps.objects) continue; ++o; break;
        }
        out.push_back(i);
    }
    return out;
}

void ValidateResult(const std::vector<BudgetCandidate>& in,
                    const BudgetCaps& caps,
                    const std::vector<int>& out)
{
    int p = 0, v = 0, o = 0;
    std::vector<bool> seen(in.size(), false);
    bool seenNonMission = false;
    for (int idx : out)
    {
        if (idx < 0 || static_cast<size_t>(idx) >= in.size())
        {
            ++g_failures;
            return;
        }
        if (seen[static_cast<size_t>(idx)])
        {
            ++g_failures;
            return;
        }
        seen[static_cast<size_t>(idx)] = true;
        const auto& c = in[static_cast<size_t>(idx)];
        if (c.mission && seenNonMission)
        {
            ++g_failures; // mission must come first
            return;
        }
        if (!c.mission) seenNonMission = true;
        switch (c.kind)
        {
        case BudgetKind::Ped: ++p; break;
        case BudgetKind::Vehicle: ++v; break;
        case BudgetKind::Object: ++o; break;
        }
    }
    if (p > caps.peds || v > caps.vehicles || o > caps.objects)
        ++g_failures;
    if (out.size() > static_cast<size_t>(caps.peds + caps.vehicles + caps.objects))
        ++g_failures;
}

void Worker(unsigned seed, std::atomic<int>& failures)
{
    std::mt19937 rng(seed);
    for (int trial = 0; trial < 10000; ++trial)
    {
        const size_t n = rng() % 301;
        std::vector<BudgetCandidate> in(n);
        for (auto& c : in)
        {
            c.kind = static_cast<BudgetKind>(rng() % 3);
            c.mission = (rng() % 5) == 0;
            c.distance2 = static_cast<float>(rng() % 100000) * 0.01f;
        }
        BudgetCaps caps;
        caps.peds = static_cast<int>(rng() % 40);
        caps.vehicles = static_cast<int>(rng() % 20);
        caps.objects = static_cast<int>(rng() % 20);

        const auto got = coop::world_sync::SelectWorldCandidates(in, caps);
        const auto expected = Reference(in, caps);
        if (got != expected)
        {
            ++failures;
            return;
        }
        ValidateResult(in, caps, got);
    }
}

} // namespace

int main()
{
    // Edge cases.
    {
        BudgetCaps caps;
        Check(coop::world_sync::SelectWorldCandidates({}, caps).empty(),
              "empty input");
    }
    {
        std::vector<BudgetCandidate> in(3);
        in[0] = {BudgetKind::Ped, false, 1.f};
        in[1] = {BudgetKind::Vehicle, true, 9.f};
        in[2] = {BudgetKind::Object, false, 2.f};
        BudgetCaps caps{1, 1, 1};
        auto out = coop::world_sync::SelectWorldCandidates(in, caps);
        Check(out.size() == 3, "all selected with caps 1/1/1");
        Check(out[0] == 1, "mission vehicle first");
    }
    {
        // Mission priority wins over distance; per-kind caps are applied after
        // the global (mission, distance) ordering.
        std::vector<BudgetCandidate> in(4);
        in[0] = {BudgetKind::Ped, false, 1.f};
        in[1] = {BudgetKind::Vehicle, true, 100.f};
        in[2] = {BudgetKind::Ped, true, 50.f};
        in[3] = {BudgetKind::Object, false, 2.f};
        BudgetCaps caps{1, 1, 1};
        auto out = coop::world_sync::SelectWorldCandidates(in, caps);
        Check(out == std::vector<int>({2, 1, 3}),
              "mission first, then bounded per-kind picks");
        Check(coop::world_sync::SelectWorldCandidates(in, BudgetCaps{0, 0, 0}).empty(),
              "zero caps select nothing");
    }
    {
        std::vector<BudgetCandidate> in(5);
        for (int i = 0; i < 5; ++i)
            in[i] = {BudgetKind::Ped, false, (float)i};
        BudgetCaps caps{2, 0, 0};
        auto out = coop::world_sync::SelectWorldCandidates(in, caps);
        Check(out.size() == 2 && out[0] == 0 && out[1] == 1,
              "nearest peds selected, cap enforced");
    }

    // Multithreaded stress: 4 x 10k random candidate sets vs reference.
    {
        std::atomic<int> threadFailures{0};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i)
            threads.emplace_back(Worker, 900u + i, std::ref(threadFailures));
        for (auto& t : threads) t.join();
        g_failures += threadFailures.load();
        std::printf("world_entity_budget_test: multithreaded stress pass\n");
    }

    if (g_failures == 0)
    {
        std::printf("world_entity_budget_test: PASS\n");
        return 0;
    }
    std::printf("world_entity_budget_test: %d failure(s)\n", g_failures);
    return 1;
}
