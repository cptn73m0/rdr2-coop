#pragma once

#include <cstdint>
#include <vector>

namespace coop {
namespace world_sync {

// Чистая (без нативов) функция выбора сущностей для WorldSnapshot:
// mission-приоритет, затем дистанция, затем отдельные лимиты Ped/Vehicle/Object.
enum class BudgetKind : uint8_t
{
    Ped = 0,
    Vehicle = 1,
    Object = 2,
};

struct BudgetCandidate
{
    BudgetKind kind = BudgetKind::Ped;
    bool mission = false;
    float distance2 = 0.f;
};

struct BudgetCaps
{
    int peds = 32;
    int vehicles = 16;
    int objects = 16;
};

// Возвращает индексы выбранных кандидатов в порядке приоритета.
std::vector<int> SelectWorldCandidates(
    const std::vector<BudgetCandidate>& candidates,
    const BudgetCaps& caps);

} // namespace world_sync
} // namespace coop
