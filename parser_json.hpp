#pragma once
#ifndef JSON_PARSER_HPP
#define JSON_PARSER_HPP

#include "deployment_plan.hpp"
#include <vector>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <sstream>
#include <stdexcept>


class JsonParser {
public:
    DeploymentPlan parse(const std::string& filename);

private:

    std::vector<HostInfo>       parse_hosts(const nlohmann::json& j);
    std::vector<TaskInfo>       parse_tasks(const nlohmann::json& j);
    std::vector<SubtaskInfo>    parse_subtasks(const nlohmann::json& j);
    std::vector<ConnectionInfo> parse_connections(const nlohmann::json& j);
};

#endif // JSON_PARSER_HPP