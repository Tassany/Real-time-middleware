#include <iostream>
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
    return 0;
}