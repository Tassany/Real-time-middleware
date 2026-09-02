#pragma once
#ifndef ALLOCATOR_HPP
#define ALLOCATOR_HPP

#include "deployment_plan.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <cmath>
#include <map>
#include <unistd.h>

// Result of a bin-packing allocation.
// subtasks: copy of the input with 'core' field set (-1 if couldn't be placed).
//           NOTE: sorting permutes this vector — match entries by .id, never by
//           position against the input.
// core_util: load accumulated per core. Under WeightMode::Utilization this is
//            utilization (wcet_ns / period_ns per subtask); under
//            WeightMode::Unit it is simply the subtask count.
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

// How much "room" a subtask consumes on a core.
//
// Unit exists because utilization needs wcet_ns, which the deployment plans do
// not carry. With every utilization at 0.0 the fit tests below would see every
// core as equally empty and pile the whole task set onto core 0. Weighing each
// subtask as 1.0 instead makes the load metric a subtask count, which turns
// worst_fit into "put it on the least-loaded core".
enum class WeightMode {
    Utilization,
    Unit,
};

inline double weight(const SubtaskInfo& s, WeightMode mode) {
    return (mode == WeightMode::Unit) ? 1.0 : utilization(s);
}

// Task sorting criteria (Lupu et al. 2010, Sec. 3.2), restricted to the
// fields available without a cycles->time conversion: raw period and
// utilization (wcet_ns/period_ns). Deadline and density are not offered
// separately because every subtask in this project uses an implicit
// deadline (deadline_ns == period_ns), so they would be identical to
// PeriodAsc/Desc and UtilizationAsc/Desc respectively.
//
// Priority* order by the declared 'priority' field. Under the rate-monotonic
// assignment used by the plans this coincides with PeriodAsc/Desc, but it reads
// the programmer's stated intent rather than re-deriving it from the period.
enum class SortCriterion {
    None,
    PeriodAsc,
    PeriodDesc,
    UtilizationAsc,
    UtilizationDesc,
    PriorityDesc,
    PriorityAsc,
};

enum class Strategy {
    FirstFit,
    BestFit,
    WorstFit,
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
        case SortCriterion::PriorityDesc:
            std::stable_sort(subtasks.begin(), subtasks.end(),
                [](const SubtaskInfo& a, const SubtaskInfo& b) { return a.priority > b.priority; });
            break;
        case SortCriterion::PriorityAsc:
            std::stable_sort(subtasks.begin(), subtasks.end(),
                [](const SubtaskInfo& a, const SubtaskInfo& b) { return a.priority < b.priority; });
            break;
    }
}

namespace detail {

// Picks the core a subtask of load u goes on, or -1 if it fits nowhere.
inline int choose_core(Strategy strat, const std::vector<double>& core_util,
                       double u, double capacity) {
    const int num_cores = static_cast<int>(core_util.size());

    if (strat == Strategy::FirstFit) {
        for (int c = 0; c < num_cores; ++c)
            if (core_util[c] + u <= capacity) return c;
        return -1;
    }

    // Best Fit takes the tightest remaining room that still fits, Worst Fit the
    // largest. Both scan with a strict comparison, so ties go to the lowest core
    // index — that is what makes a Unit-weight Worst Fit round-robin.
    int    chosen    = -1;
    double best_room = (strat == Strategy::BestFit)
                     ? std::numeric_limits<double>::max()
                     : -1.0;

    for (int c = 0; c < num_cores; ++c) {
        double remaining = capacity - core_util[c];
        if (remaining < u) continue;
        bool better = (strat == Strategy::BestFit) ? (remaining < best_room)
                                                   : (remaining > best_room);
        if (better) { best_room = remaining; chosen = c; }
    }
    return chosen;
}

// Shared packing loop. initial_util seeds the cores (e.g. with subtasks already
// pinned by hand); pass an empty vector to start from zero.
inline AllocationResult pack(std::vector<SubtaskInfo> subtasks,
                             Strategy strat,
                             int num_cores,
                             double capacity,
                             SortCriterion sort_by,
                             WeightMode mode,
                             const std::vector<double>& initial_util) {
    sort_subtasks(subtasks, sort_by);

    AllocationResult r;
    r.subtasks = std::move(subtasks);
    r.feasible = true;
    if (initial_util.empty()) r.core_util.assign(num_cores, 0.0);
    else                      r.core_util = initial_util;

    for (auto& s : r.subtasks) {
        double u = weight(s, mode);
        int    c = choose_core(strat, r.core_util, u, capacity);
        if (c >= 0) { s.core = c; r.core_util[c] += u; }
        else        { s.core = -1; r.feasible = false; }
    }
    return r;
}

} // namespace detail

// First Fit: place each subtask on the first core that has room.
inline AllocationResult first_fit(std::vector<SubtaskInfo> subtasks,
                                  int num_cores,
                                  double capacity = 1.0,
                                  SortCriterion sort_by = SortCriterion::None,
                                  WeightMode mode = WeightMode::Utilization) {
    return detail::pack(std::move(subtasks), Strategy::FirstFit, num_cores,
                        capacity, sort_by, mode, {});
}

// Best Fit: place each subtask on the core with the least remaining room that still fits.
inline AllocationResult best_fit(std::vector<SubtaskInfo> subtasks,
                                 int num_cores,
                                 double capacity = 1.0,
                                 SortCriterion sort_by = SortCriterion::None,
                                 WeightMode mode = WeightMode::Utilization) {
    return detail::pack(std::move(subtasks), Strategy::BestFit, num_cores,
                        capacity, sort_by, mode, {});
}

// Worst Fit: place each subtask on the core with the most remaining room.
inline AllocationResult worst_fit(std::vector<SubtaskInfo> subtasks,
                                  int num_cores,
                                  double capacity = 1.0,
                                  SortCriterion sort_by = SortCriterion::None,
                                  WeightMode mode = WeightMode::Utilization) {
    return detail::pack(std::move(subtasks), Strategy::WorstFit, num_cores,
                        capacity, sort_by, mode, {});
}

// -----------------------------------------------------------------------
//  Automatic allocation driven by the plan's "allocation" block
// -----------------------------------------------------------------------

inline std::vector<SubtaskInfo> flatten(const DeploymentPlan& plan) {
    std::vector<SubtaskInfo> all;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            all.push_back(st);
    return all;
}

namespace detail {

inline Strategy parse_strategy(const std::string& v) {
    if (v == "first_fit") return Strategy::FirstFit;
    if (v == "best_fit")  return Strategy::BestFit;
    if (v == "worst_fit") return Strategy::WorstFit;
    throw std::runtime_error("allocation.strategy: unknown value '" + v +
                             "' (expected first_fit | best_fit | worst_fit)");
}

inline SortCriterion parse_sort_by(const std::string& v) {
    if (v == "none")             return SortCriterion::None;
    if (v == "period_asc")       return SortCriterion::PeriodAsc;
    if (v == "period_desc")      return SortCriterion::PeriodDesc;
    if (v == "utilization_asc")  return SortCriterion::UtilizationAsc;
    if (v == "utilization_desc") return SortCriterion::UtilizationDesc;
    if (v == "priority_desc")    return SortCriterion::PriorityDesc;
    if (v == "priority_asc")     return SortCriterion::PriorityAsc;
    throw std::runtime_error("allocation.sort_by: unknown value '" + v + "'");
}

inline WeightMode parse_weight(const std::string& v) {
    if (v == "count")       return WeightMode::Unit;
    if (v == "utilization") return WeightMode::Utilization;
    throw std::runtime_error("allocation.weight: unknown value '" + v +
                             "' (expected count | utilization)");
}

} // namespace detail

// Assigns a core to every subtask left at CORE_UNASSIGNED, honouring the ones
// that declare a core as fixed pinning. Throws if the plan is inconsistent or
// if some subtask cannot be placed.
inline void apply_auto_allocation(DeploymentPlan& plan) {
    const AllocationConfig& cfg = plan.allocation;

    const Strategy      strat = detail::parse_strategy(cfg.strategy);
    const SortCriterion sort  = detail::parse_sort_by(cfg.sort_by);
    const WeightMode    mode  = detail::parse_weight(cfg.weight);

    const int online = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    int num_cores = (cfg.num_cores > 0) ? cfg.num_cores : online;
    if (num_cores <= 0)
        throw std::runtime_error("allocation.num_cores: must be > 0");
    // Dispatcher ignores the pthread_setaffinity_np return code, so a core that
    // does not exist would silently let the thread float across every CPU.
    if (num_cores > online)
        throw std::runtime_error("allocation.num_cores: " + std::to_string(num_cores) +
                                 " exceeds the " + std::to_string(online) +
                                 " online CPUs");

    // Split pinned from free, seeding the cores with the pinned load.
    const std::vector<SubtaskInfo> all = flatten(plan);
    std::vector<SubtaskInfo> free;
    std::vector<double>      seed(num_cores, 0.0);

    for (const auto& s : all) {
        if (s.core == CORE_UNASSIGNED) { free.push_back(s); continue; }
        if (s.core < 0 || s.core >= num_cores)
            throw std::runtime_error("subtask " + std::to_string(s.id) +
                                     ": pinned core " + std::to_string(s.core) +
                                     " outside [0, " + std::to_string(num_cores) + ")");
        seed[s.core] += weight(s, mode);
    }

    if (free.empty()) return;

    double capacity = cfg.capacity;
    if (capacity <= 0.0) {
        capacity = (mode == WeightMode::Unit)
                 ? std::ceil(static_cast<double>(all.size()) / num_cores)
                 : 1.0;
    }

    AllocationResult r = detail::pack(std::move(free), strat, num_cores,
                                      capacity, sort, mode, seed);

    if (!r.feasible) {
        std::string ids;
        for (const auto& s : r.subtasks)
            if (s.core < 0) ids += (ids.empty() ? "" : ", ") + std::to_string(s.id);
        throw std::runtime_error("automatic allocation infeasible on " +
                                 std::to_string(num_cores) + " cores; unplaced subtask(s): " + ids);
    }

    // Write back by id: pack() sorted its copy, so positions no longer line up.
    std::map<int, int> core_of;
    for (const auto& s : r.subtasks) core_of[s.id] = s.core;

    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (st.core == CORE_UNASSIGNED) st.core = core_of.at(st.id);
}

} // namespace allocator

#endif // ALLOCATOR_HPP
