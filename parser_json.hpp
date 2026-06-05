#pragma once
#ifndef JSON_PARSER_HPP
#define JSON_PARSER_HPP

/**
 * @file parser_json.hpp
 * @brief JSON parser that deserialises a deployment plan configuration file.
 *
 * The expected JSON format is:
 * @code
 * {
 *   "hosts":       [ { "name": "...", "address": "..." } ],
 *   "tasks":       [ { "name": "...", "subtasks": [ ... ] } ],
 *   "connections": [ { "upstream": "...", "downstream": "..." } ]
 * }
 * @endcode
 *
 * Relies on the nlohmann/json single-header library (include/nlohmann/json.hpp).
 */

#include "deployment_plan.hpp"
#include <vector>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <sstream>
#include <stdexcept>


/**
 * @brief Deserialises a deployment_plan.json file into a DeploymentPlan struct.
 *
 * Usage:
 * @code
 *   JsonParser parser;
 *   DeploymentPlan plan = parser.parse("deployment_plan.json");
 * @endcode
 */
class JsonParser {
public:
    /**
     * @brief Open and parse a JSON deployment plan file.
     * @param filename Path to the JSON file.
     * @return Populated DeploymentPlan ready for use by the runtime.
     * @throws std::runtime_error if the file cannot be opened.
     * @throws nlohmann::json::parse_error if the JSON is malformed.
     */
    DeploymentPlan parse(const std::string& filename);

private:
    std::vector<HostInfo>       parse_hosts(const nlohmann::json& j);
    std::vector<TaskInfo>       parse_tasks(const nlohmann::json& j);
    std::vector<SubtaskInfo>    parse_subtasks(const nlohmann::json& j);
    std::vector<ConnectionInfo> parse_connections(const nlohmann::json& j);
};

#endif // JSON_PARSER_HPP