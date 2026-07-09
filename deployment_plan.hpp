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


struct SubtaskInfo {
    int         task_id = 0;     // parent task; set by parser, 0 for manually-built entries
    int         id      = 0;    // subtask (globally unique across all tasks)
    std::string component_type;
    std::string host;
    int         core;
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

struct DeploymentPlan {
    std::vector<HostInfo>       hosts;
    std::vector<TaskInfo>       tasks;
    std::vector<ConnectionInfo> connections;
};

#endif // DEPLOYMENT_PLAN_HPP