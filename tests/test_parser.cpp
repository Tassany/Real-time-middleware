#include <cassert>
#include <iostream>
#include <fstream>
#include "../parser_json.hpp"

// Helper: cria um JSON temporário com o conteúdo fornecido
static void write_tmp(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static void test_parser_period_deadline() {
    JsonParser parser;
    DeploymentPlan plan = parser.parse("deployment_plan.json");

    assert(!plan.tasks.empty());
    const auto& subtasks = plan.tasks[0].subtasks;
    assert(subtasks.size() == 4);

    assert(subtasks[0].period_ns   == 100000000ULL);
    assert(subtasks[0].deadline_ns == 100000000ULL);
    assert(subtasks[1].period_ns   == 200000000ULL);
    assert(subtasks[1].deadline_ns == 200000000ULL);
    assert(subtasks[2].period_ns   == 300000000ULL);
    assert(subtasks[2].deadline_ns == 300000000ULL);
    assert(subtasks[3].period_ns   == 400000000ULL);
    assert(subtasks[3].deadline_ns == 400000000ULL);

    std::cout << "[PASS] test_parser_period_deadline\n";
}

static void test_parser_defaults() {
    const std::string tmp = "/tmp/test_plan_no_timing.json";
    write_tmp(tmp, R"({
        "hosts": [{"name": "h1", "address": "127.0.0.1"}],
        "tasks": [{
            "id": 1,
            "subtasks": [{
                "id": 1,
                "component_type": "source",
                "host": "h1",
                "core": 0,
                "priority": 1
            }]
        }],
        "connections": []
    })");

    JsonParser parser;
    DeploymentPlan plan = parser.parse(tmp);

    assert(!plan.tasks.empty());
    const auto& s = plan.tasks[0].subtasks[0];
    assert(s.period_ns   == 0);
    assert(s.deadline_ns == 0);

    std::cout << "[PASS] test_parser_defaults\n";
}

static void test_parser_all_fields() {
    const std::string tmp = "/tmp/test_plan_all_fields.json";
    write_tmp(tmp, R"({
        "hosts": [{"name": "server", "address": "10.0.0.1"}],
        "tasks": [{
            "id": 42,
            "subtasks": [{
                "id": 7,
                "component_type": "intermediate",
                "host": "server",
                "core": 3,
                "priority": 50,
                "period_ns": 5000000,
                "deadline_ns": 4500000
            }]
        }],
        "connections": [{"upstream": 1, "downstream": 2}]
    })");

    JsonParser parser;
    DeploymentPlan plan = parser.parse(tmp);

    assert(plan.tasks[0].id == 42);
    const auto& s = plan.tasks[0].subtasks[0];
    assert(s.id             == 7);
    assert(s.component_type == "intermediate");
    assert(s.host           == "server");
    assert(s.core           == 3);
    assert(s.priority       == 50);
    assert(s.period_ns      == 5000000ULL);
    assert(s.deadline_ns    == 4500000ULL);
    assert(plan.connections[0].upstream   == 1);
    assert(plan.connections[0].downstream == 2);

    std::cout << "[PASS] test_parser_all_fields\n";
}

int main() {
    test_parser_period_deadline();
    test_parser_defaults();
    test_parser_all_fields();
    return 0;
}
