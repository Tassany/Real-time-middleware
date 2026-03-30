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
    std::string name;         
    std::string component;    
    std::string host;         
    int         core;         
    int         priority;   
};

struct ConnectionInfo {
    std::string upstream;   
    std::string downstream; 
};

struct TaskInfo {
    std::string             name;
    std::vector<SubtaskInfo> subtasks;
};

struct DeploymentPlan {
    std::vector<HostInfo>       hosts;
    std::vector<TaskInfo>       tasks;
    std::vector<ConnectionInfo> connections;
};

#endif // DEPLOYMENT_PLAN_HPP