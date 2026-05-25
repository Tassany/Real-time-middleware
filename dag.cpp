#include "dag.hpp"

void DAG::add_node(int id, ComponentBase* component) {
    Node new_node;
    new_node.id = id;
    new_node.component = component;
    nodes_.push_back(new_node);
}

void DAG::add_edge(int from, int to) {
    for (auto& node : nodes_) {
        if (node.id == from) {
            node.successors.push_back(to);
        }
        if (node.id == to) {
            node.predecessors.push_back(from);
        }
    }
}
