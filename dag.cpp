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

std::vector<int> DAG::topological_sort() {
    std::vector<int> sorted;
    int n = nodes_.size();
    std::queue<int> queue;
    std::vector<int> indegree(n, 0);

    //calculate in-degrees
    for (const auto& node : nodes_) {
        for (int pred : node.predecessors) {
            indegree[node.id]++;
        }
    }
    //enqueue nodes with zero in-degree
    for (const auto& node : nodes_) {
        if (indegree[node.id] == 0) {
            queue.push(node.id);
        }
    }
    // Kahn’s Algorithm
    while (!queue.empty()) {
        int current = queue.front();
        queue.pop();
        sorted.push_back(current);
        for (int succ : nodes_[current].successors) {
            indegree[succ]--;
            if (indegree[succ] == 0) {
                queue.push(succ);
            }
        }
    }
    
    return sorted;
}

bool DAG::has_cycle() {
    return false;
}