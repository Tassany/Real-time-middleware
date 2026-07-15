#pragma once
#ifndef DEPLOYMENT_PLAN_HPP
#define DEPLOYMENT_PLAN_HPP

#include <string>
#include <vector>
#include <cstdint>
#include "include/nlohmann/json.hpp"

using json = nlohmann::json;

struct HostInfo {
    std::string name;
    std::string address;
};

// 'core' sentinel: no core declared in the plan, to be filled by the allocator.
// The allocator also uses -1 to mark "could not place", so any core still < 0
// after allocation is an error either way.
constexpr int CORE_UNASSIGNED = -1;


struct SubtaskInfo {
    int         task_id = 0;     // parent task; set by parser, 0 for manually-built entries
    int         id      = 0;    // subtask (globally unique across all tasks)
    std::string component_type;
    std::string host;
    int         core = CORE_UNASSIGNED;
    int         priority;
    uint64_t    period_ns;
    uint64_t    deadline_ns;
    uint64_t    wcet_ns = 0;    // worst-case execution time; 0 = not set
    json        config;
};

struct ConnectionInfo {
    int upstream;   
    int downstream; 
};

struct TaskInfo {
    int id;
    std::vector<SubtaskInfo> subtasks;
};

// Optional "allocation" block of the plan. Drives the automatic core
// assignment applied to every subtask that omits "core".
struct AllocationConfig {
    std::string strategy  = "worst_fit";     // first_fit | best_fit | worst_fit
    std::string sort_by   = "priority_desc"; // priority_desc | priority_asc | period_asc |
                                             // period_desc | utilization_asc |
                                             // utilization_desc | none
    std::string weight    = "count";         // count | utilization
    int         num_cores = 0;               // 0 = all online CPUs
    double      capacity  = 0.0;             // 0 = derive from weight mode
};

struct DeploymentPlan {
    std::vector<HostInfo>       hosts;
    std::vector<TaskInfo>       tasks;
    std::vector<ConnectionInfo> connections;
    AllocationConfig            allocation;
};

#endif // DEPLOYMENT_PLAN_HPP