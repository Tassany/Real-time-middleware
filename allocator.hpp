#pragma once
#ifndef ALLOCATOR_HPP
#define ALLOCATOR_HPP

#include "deployment_plan.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <limits>
#include <algorithm>

// Result of a bin-packing allocation.
// subtasks: copy of the input with 'core' field set (-1 if couldn't be placed).
// core_util: utilization accumulated per core (wcet_ns / period_ns per subtask).
// feasible: true if every subtask was placed within capacity.
struct AllocationResult {
    std::vector<SubtaskInfo> subtasks;
    std::vector<double>      core_util;
    bool                     feasible;
};

namespace allocator {

inline double utilization(const SubtaskInfo& s) {
    if (s.period_ns == 0) return 0.0;
    return static_cast<double>(s.wcet_ns) / static_cast<double>(s.period_ns);
}

// Task sorting criteria (Lupu et al. 2010, Sec. 3.2), restricted to the
// fields available without a cycles->time conversion: raw period and
// utilization (wcet_ns/period_ns). Deadline and density are not offered
// separately because every subtask in this project uses an implicit
// deadline (deadline_ns == period_ns), so they would be identical to
// PeriodAsc/Desc and UtilizationAsc/Desc respectively.
enum class SortCriterion {
    None,
    PeriodAsc,
    PeriodDesc,
    UtilizationAsc,
    UtilizationDesc,
};

// Orders subtasks in place according to crit. Stable so that ties keep
// their original (arrival) order, matching the paper's "no criterion"
// baseline when applied to an already-tied task set.
inline void sort_subtasks(std::vector<SubtaskInfo>& subtasks, SortCriterion crit) {
    switch (crit) {
        case SortCriterion::None:
            break;
        case SortCriterion::PeriodAsc:
            std::stable_sort(subtasks.begin(), subtasks.end(),
                [](const SubtaskInfo& a, const SubtaskInfo& b) { return a.period_ns < b.period_ns; });
            break;
        case SortCriterion::PeriodDesc:
            std::stable_sort(subtasks.begin(), subtasks.end(),
                [](const SubtaskInfo& a, const SubtaskInfo& b) { return a.period_ns > b.period_ns; });
            break;
        case SortCriterion::UtilizationAsc:
            std::stable_sort(subtasks.begin(), subtasks.end(),
                [](const SubtaskInfo& a, const SubtaskInfo& b) { return utilization(a) < utilization(b); });
            break;
        case SortCriterion::UtilizationDesc:
            std::stable_sort(subtasks.begin(), subtasks.end(),
                [](const SubtaskInfo& a, const SubtaskInfo& b) { return utilization(a) > utilization(b); });
            break;
    }
}

// First Fit: place each subtask on the first core that has room.
inline AllocationResult first_fit(std::vector<SubtaskInfo> subtasks,
                                  int num_cores,
                                  double capacity = 1000.0,
                                  SortCriterion sort_by = SortCriterion::None) {
    sort_subtasks(subtasks, sort_by);
    AllocationResult r;
    r.subtasks   = subtasks;
    r.core_util.assign(num_cores, 0.0);
    r.feasible   = true;

    for (auto& s : r.subtasks) {
        double u = utilization(s);
        bool placed = false;
        for (int c = 0; c < num_cores; ++c) {
            if (r.core_util[c] + u <= capacity) {
                s.core = c;
                r.core_util[c] += u;
                placed = true;
                break;
            }
        }
        if (!placed) { s.core = -1; r.feasible = false; }
    }
    return r;
}

// Best Fit: place each subtask on the core with the least remaining room that still fits.
inline AllocationResult best_fit(std::vector<SubtaskInfo> subtasks,
                                 int num_cores,
                                 double capacity = 1000.0,
                                 SortCriterion sort_by = SortCriterion::None) {
    sort_subtasks(subtasks, sort_by);
    AllocationResult r;
    r.subtasks   = subtasks;
    r.core_util.assign(num_cores, 0.0);
    r.feasible   = true;

    for (auto& s : r.subtasks) {
        double u = utilization(s);
        int    best = -1;
        double best_remaining = std::numeric_limits<double>::max();

        for (int c = 0; c < num_cores; ++c) {
            double remaining = capacity - r.core_util[c];
            if (remaining >= u && remaining < best_remaining) {
                best_remaining = remaining;
                best = c;
            }
        }
        if (best >= 0) { s.core = best; r.core_util[best] += u; }
        else           { s.core = -1;   r.feasible = false;     }
    }
    return r;
}

// Worst Fit: place each subtask on the core with the most remaining room.
inline AllocationResult worst_fit(std::vector<SubtaskInfo> subtasks,
                                  int num_cores,
                                  double capacity = 1000.0,
                                  SortCriterion sort_by = SortCriterion::None) {
    sort_subtasks(subtasks, sort_by);
    AllocationResult r;
    r.subtasks   = subtasks;
    r.core_util.assign(num_cores, 0.0);
    r.feasible   = true;

    for (auto& s : r.subtasks) {
        double u = utilization(s);
        int    worst = -1;
        double worst_remaining = -1.0;

        for (int c = 0; c < num_cores; ++c) {
            double remaining = capacity - r.core_util[c];
            if (remaining >= u && remaining > worst_remaining) {
                worst_remaining = remaining;
                worst = c;
            }
        }
        if (worst >= 0) { s.core = worst; r.core_util[worst] += u; }
        else            { s.core = -1;    r.feasible = false;      }
    }
    return r;
}

} // namespace allocator

#endif // ALLOCATOR_HPP
