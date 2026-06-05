#include "dag.hpp"
#include <unordered_map>
#include <stdexcept>
#include <algorithm>

void DAG::add_node(int id, ComponentBase* component) {
    Node n;
    n.id        = id;
    n.component = component;
    nodes_.push_back(n);
}

void DAG::add_edge(int from, int to) {
    for (auto& n : nodes_) {
        if (n.id == from) n.successors.push_back(to);
        if (n.id == to)   n.predecessors.push_back(from);
    }
}

const DAG::Node* DAG::find_node(int id) const {
    for (const auto& n : nodes_)
        if (n.id == id) return &n;
    return nullptr;
}

std::vector<int> DAG::topological_sort() const {
    
    std::unordered_map<int, int> indegree;
    // Compute indegrees (how many incoming edges each vertex has)
    for (const auto& n : nodes_)
        indegree[n.id] = static_cast<int>(n.predecessors.size());


    // Add all nodes with indegree 0 
    // into the queue
    std::queue<int> q;
    for (const auto& n : nodes_)
        if (indegree[n.id] == 0)
            q.push(n.id);

    std::vector<int> sorted;
    sorted.reserve(nodes_.size());

    // Kahn’s Algorithm (BFS)
    while (!q.empty()) {
        int cur = q.front(); q.pop();
        sorted.push_back(cur);
        const Node* node = find_node(cur);
        for (int succ : node->successors) {
            if (--indegree[succ] == 0)
                q.push(succ);
        }
    }

    if (sorted.size() < nodes_.size())
        throw std::runtime_error("DAG::topological_sort: cycle detected");

    return sorted;
}

bool DAG::has_cycle() const {
    try {
        topological_sort();
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

int DAG::pipeline_depth() const {
    std::vector<int> order = topological_sort(); // throws if cycle

    std::unordered_map<int, int> depth;
    for (const auto& n : nodes_)
        depth[n.id] = 1;

    for (int id : order) {
        const Node* node = find_node(id);
        for (int succ : node->successors)
            depth[succ] = std::max(depth[succ], depth[id] + 1);
    }

    int max_depth = 0;
    for (const auto& [id, d] : depth)
        max_depth = std::max(max_depth, d);

    return max_depth;
}

int DAG::fan_in_count(int id) const {
    const Node* n = find_node(id);
    return n ? static_cast<int>(n->predecessors.size()) : -1;
}

int DAG::fan_out_count(int id) const {
    const Node* n = find_node(id);
    return n ? static_cast<int>(n->successors.size()) : -1;
}
