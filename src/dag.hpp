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

    // Returns node IDs in a valid execution order (Kahn's algorithm).
    // Throws std::runtime_error if the graph has a cycle.
    std::vector<int> topological_sort() const;

    // Returns true if the graph contains a cycle.
    bool has_cycle() const;

    // Returns the length of the longest path (number of nodes).
    // Throws std::runtime_error if the graph has a cycle.
    int pipeline_depth() const;

    // Number of direct predecessors of node `id`. Returns -1 if not found.
    int fan_in_count(int id) const;

    // Number of direct successors of node `id`. Returns -1 if not found.
    int fan_out_count(int id) const;

    // Read-only access to all nodes (used by TeamManager for wiring).
    const std::vector<Node>& nodes() const { return nodes_; }

private:
    std::vector<Node> nodes_; // List of subtasks (nodes) in the DAG


    const Node* find_node(int id) const;
};

#endif // DAG_HPP
