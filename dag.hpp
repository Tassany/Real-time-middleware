#ifndef DAG_HPP
#define DAG_HPP

#include <vector>
#include <queue>
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
    bool has_cycle();
    std::vector<int> topological_sort();

private:
    std::vector<Node> nodes_; // List of subtasks (nodes) in the DAG
};

#endif // DAG_HPP
