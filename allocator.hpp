#pragma once
#ifndef ALLOCATOR_HPP
#define ALLOCATOR_HPP

#include "deployment_plan.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <limits>

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

// First Fit: place each subtask on the first core that has room.
inline AllocationResult first_fit(std::vector<SubtaskInfo> subtasks,
                                  int num_cores,
                                  double capacity = 1.0) {
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
                                 double capacity = 1.0) {
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
                                  double capacity = 1.0) {
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
