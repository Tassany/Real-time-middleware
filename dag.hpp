#ifndef DAG_HPP
#define DAG_HPP

#include <vector>
#include "component.hpp"

class DAG {
public:
    struct Node {
        int id;
        std::vector<int> predecessors;
        std::vector<int> successors; 
        ComponentBase* component;
    };

    DAG() {}
    void add_node(int id, ComponentBase* component);
    void add_edge(int from, int to);

private:
    std::vector<Node> nodes_;
};

#endif // DAG_HPP
