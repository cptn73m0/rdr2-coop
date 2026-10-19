#include "world_entity_budget.h"

#include <algorithm>

namespace coop {
namespace world_sync {

std::vector<int> SelectWorldCandidates(
    const std::vector<BudgetCandidate>& candidates,
    const BudgetCaps& caps)
{
    std::vector<int> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int>(i);

    std::stable_sort(order.begin(), order.end(),
        [&](int a, int b)
        {
            const BudgetCandidate& ca = candidates[a];
            const BudgetCandidate& cb = candidates[b];
            if (ca.mission != cb.mission)
                return ca.mission; // mission first
            return ca.distance2 < cb.distance2;
        });

    int peds = 0;
    int vehicles = 0;
    int objects = 0;
    std::vector<int> selected;
    selected.reserve(order.size());
    for (const int index : order)
    {
        switch (candidates[index].kind)
        {
        case BudgetKind::Ped:
            if (peds >= caps.peds) continue;
            ++peds;
            break;
        case BudgetKind::Vehicle:
            if (vehicles >= caps.vehicles) continue;
            ++vehicles;
            break;
        case BudgetKind::Object:
            if (objects >= caps.objects) continue;
            ++objects;
            break;
        }
        selected.push_back(index);
    }
    return selected;
}

} // namespace world_sync
} // namespace coop
