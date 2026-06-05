#include "dag.hpp"

void DAG::addNode(int id, ComponentBase* component) {
    Node new_node;
    new_node.id        = id;
    new_node.component = component;
    nodes_.push_back(new_node);
}

void DAG::addEdge(int from, int to) {
    // A single pass updates both the producer's successor list and
    // the consumer's predecessor list, keeping them always in sync.
    for (auto& node : nodes_) {
        if (node.id == from) node.successors.push_back(to);
        if (node.id == to)   node.predecessors.push_back(from);
    }
}

std::vector<DAG::Node> DAG::getNodes() const {
    return nodes_;
}