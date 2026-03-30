#include "parser_json.hpp"


using json = nlohmann::json;

DeploymentPlan JsonParser::parse(const std::string& filename){
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        throw std::runtime_error("Not possible to open: " + filename);
    }

    json j = json::parse(file);

    DeploymentPlan plan;
    plan.hosts       = parse_hosts(j);
    plan.tasks       = parse_tasks(j);
    plan.connections = parse_connections(j);

    return plan;
}

std::vector<HostInfo> JsonParser::parse_hosts(const json& j) {
    std::vector<HostInfo> hosts;
    for (const auto& h : j["hosts"]) {
        HostInfo host;
        host.name    = h["name"];
        host.address = h["address"];
        hosts.push_back(host);
    }
    return hosts;
}

std::vector<TaskInfo> JsonParser::parse_tasks(const json& j) {
    std::vector<TaskInfo> tasks;
    for (const auto& t : j["tasks"]) {
        TaskInfo task;
        task.name     = t["name"];
        task.subtasks = parse_subtasks(t);
        tasks.push_back(task);
    }
    return tasks;
}

std::vector<SubtaskInfo> JsonParser::parse_subtasks(const json& j) {
    std::vector<SubtaskInfo> subtasks;
    for (const auto& s : j["subtasks"]) {
        SubtaskInfo subtask;
        subtask.name      = s["name"];
        subtask.component = s["component"];
        subtask.host      = s["host"];
        subtask.core      = s["core"];
        subtask.priority  = s["priority"];
        subtasks.push_back(subtask);
    }
    return subtasks;
}

std::vector<ConnectionInfo> JsonParser::parse_connections(const json& j) {
    std::vector<ConnectionInfo> connections;
    for(const auto& c : j["connections"]){
        ConnectionInfo connection;
        connection.upstream   = c["upstream"];
        connection.downstream = c["downstream"];
        connections.push_back(connection);
    }
    return connections;
}