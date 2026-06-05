#pragma once
#ifndef DEPLOYMENT_PLAN_HPP
#define DEPLOYMENT_PLAN_HPP

#include <string>
#include <vector>


struct HostInfo {
    std::string name;
    std::string address;
};


struct SubtaskInfo {
    int        id;      
    std::string component_type;    
    std::string host;         
    int         core;         
    int         priority;  
    uint64_t    period_ns;
    uint64_t    deadline_ns; 
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