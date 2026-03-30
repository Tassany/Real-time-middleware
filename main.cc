#include <iostream>
#include "parser_json.hpp"
#include "component.hpp"
#include "adapter.hpp"


struct config_t {
    int         task_id  = 0;
    int         core_id     = 0;
    int         priority    = 1;
    long        deadline = 0; 
};

class ComponentA : public Component<int, double, config_t> {
public:
    ComponentA(const config_t* config) : Component(config) {}
    void execute() override {
        output_ = input_ * 2.0; 
    }
};

class ComponentB : public Component<int, int, config_t> {
public:
    ComponentB(const config_t* config) : Component(config) {}
    void execute() override {
        output_ = input_ + 5;
    }
};

int double_to_int(const double & value) {
    return static_cast<int>(value);
}

int main() {

    JsonParser parser;
    DeploymentPlan plan = parser.parse("deployment_plan.json");

    config_t cfg;
    cfg.task_id = 1;
    cfg.core_id = 0;
    cfg.priority = 1;
    cfg.deadline = 1000000; // 1 ms

    ComponentA compA(&cfg);
    ComponentB compB(&cfg);

    compA.init_input(10);
    compA.init_output(0.0);
    compA.execute();

    Adapter<ComponentA,ComponentB> adapter (double_to_int);

    compB.init_input(adapter.convert(compA.output_));
    compB.init_output(0);
    compB.execute();

    std::cout  << compA.output_ << " " << compB.output_ << std::endl;

    std::cout << "=== HOSTS ===" << std::endl;
    for (const auto& host : plan.hosts) {
        std::cout << host.name 
                  << " - " 
                  << host.address << std::endl;
    }

    // 3. verificando tasks e subtasks
    std::cout << "\n=== TASKS ===" << std::endl;
    for (const auto& task : plan.tasks) {
        std::cout << "task: " << task.name << std::endl;
        for (const auto& subtask : task.subtasks) {
            std::cout << "  subtask: "   << subtask.name
                      << " | component: " << subtask.component
                      << " | host: "      << subtask.host
                      << " | core: "      << subtask.core
                      << " | priority: "  << subtask.priority
                      << std::endl;
        }
    }

    // 4. verificando conexoes
    std::cout << "\n=== CONEXOES ===" << std::endl;
    for (const auto& conn : plan.connections) {
        std::cout << conn.upstream
                  << " --> "
                  << conn.downstream
                  << std::endl;
    }
    return 0;
}