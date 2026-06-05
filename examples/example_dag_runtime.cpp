#include <iostream>
#include <unistd.h>

#include "component.hpp"
#include "adapter.hpp"
#include "runtime.hpp"
#include "parser_json.hpp"

// ============================================================
//  Shared config for all components in this pipeline
// ============================================================

struct PipelineConfig {
    int pipeline_id = 1;
};

// ============================================================
//  Component definitions — match the names in deployment_plan.json
//
//  Component1: int    → double  (scale: x * 2.5)
//  Component2: double → double  (shift: x + 10.0)
//  Component3: double → int     (truncate to integer)
//  Component4: int    → int     (final consumer, prints result)
// ============================================================

class Component1 : public Component<int, double, PipelineConfig> {
public:
    explicit Component1(const PipelineConfig* cfg) : Component(cfg) {}
    void execute() override {
        output_ = input_ * 2.5;
        std::cout << "  [T1 core 0] input=" << input_
                  << "  output=" << output_ << "\n";
    }
};

class Component2 : public Component<double, double, PipelineConfig> {
public:
    explicit Component2(const PipelineConfig* cfg) : Component(cfg) {}
    void execute() override {
        output_ = input_ + 10.0;
        std::cout << "  [T2 core 1] input=" << input_
                  << "  output=" << output_ << "\n";
    }
};

// Component3 expects int input — the Adapter<Component2,Component3>
// converts double → int (truncation), showing a real type conversion.
class Component3 : public Component<int, double, PipelineConfig> {
public:
    explicit Component3(const PipelineConfig* cfg) : Component(cfg) {}
    void execute() override {
        output_ = input_ * 3.0;
        std::cout << "  [T3 core 2] input=" << input_
                  << "  output=" << output_ << "\n";
    }
};

class Component4 : public Component<double, double, PipelineConfig> {
public:
    explicit Component4(const PipelineConfig* cfg) : Component(cfg) {}
    void execute() override {
        output_ = input_;
        std::cout << "  [T4 core 3] result=" << output_ << "\n";
    }
};

// ============================================================
//  main
// ============================================================

int main() {

    PipelineConfig cfg;

    Component1 comp1(&cfg);   
    Component2 comp2(&cfg);
    Component3 comp3(&cfg);
    Component4 comp4(&cfg);

    // Type-safe adapters between adjacent components
    //   T1(double) → T2(double) : identity
    //   T2(double) → T3(int)    : truncation  ← real type conversion
    //   T3(double) → T4(double) : identity
    Adapter<Component1, Component2> a12([](const double& v) { return v; });
    Adapter<Component2, Component3> a23([](const double& v) { return static_cast<int>(v); });
    Adapter<Component3, Component4> a34([](const double& v) { return v; });

    // --- Parse the deployment plan from JSON ---
    JsonParser parser;
    DeploymentPlan plan = parser.parse("deployment_plan.json");

    // --- Configure the Runtime ---
    Runtime rt("server");

    // Pass ComponentBase* directly — no need to wrap execute() manually
    rt.register_component("Component1", &comp1);
    rt.register_component("Component2", &comp2);
    rt.register_component("Component3", &comp3);
    rt.register_component("Component4", &comp4);

    // Register one transfer function per connection:
    // called after upstream runs and before downstream is notified
    rt.register_adapter("T1", "T2", [&]() {
        comp2.init_input(a12.convert(comp1.output_));
    });
    rt.register_adapter("T2", "T3", [&]() {
        comp3.init_input(a23.convert(comp2.output_));
    });
    rt.register_adapter("T3", "T4", [&]() {
        comp4.init_input(a34.convert(comp3.output_));
    });

    // --- Build: creates Dispatchers, Subtasks, wires the DAG ---
    std::cout << "\n=== Building from deployment plan ===\n";
    rt.build(plan);

    // --- Inspect the DAG that was automatically built ---
    std::cout << "\n=== DAG topology ===\n";
    for (const auto& node : rt.dag().getNodes()) {
        std::cout << "  node " << node.id;
        if (!node.predecessors.empty()) {
            std::cout << "  predecessors: ";
            for (int p : node.predecessors) std::cout << p << " ";
        }
        if (!node.successors.empty()) {
            std::cout << "  successors: ";
            for (int s : node.successors) std::cout << s << " ";
        }
        std::cout << "\n";
    }

    // --- Start all Dispatcher threads ---
    std::cout << "\n=== Starting ===\n";
    rt.start();

    // --- Trigger 3 jobs ---
    std::cout << "\n=== Running ===\n";
    for (int job = 1; job <= 3; job++) {
        comp1.init_input(job * 4);
        std::cout << "[main] trigger T1 with input=" << job * 4 << "\n";
        rt.trigger("T1");
        sleep(1);
    }

    // --- Shutdown ---
    sleep(1);
    std::cout << "\n=== Stopping ===\n";
    rt.stop();

    return 0;
}
