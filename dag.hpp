#ifndef DAG_HPP
#define DAG_HPP

/**
 * @file dag.hpp
 * @brief Directed Acyclic Graph (DAG) representing a dependent task graph.
 *
 * Nodes correspond to Components and edges encode data-flow dependencies.
 * The DAG is used by the middleware runtime to determine execution order and
 * to propagate outputs from upstream to downstream components.
 */

#include <vector>
#include "component.hpp"

/**
 * @brief DAG of ComponentBase nodes connected by directed data-flow edges.
 *
 * Nodes are identified by integer IDs. An edge (A -> B) means A must complete
 * before B is notified to run, and A's output is the source of B's input.
 */
class DAG {
public:

    /**
     * @brief A single node in the task graph.
     *
     * predecessors lists nodes that must finish before this one is ready.
     * successors   lists nodes that become ready after this one finishes.
     */
    struct Node {
        int              id;           ///< Unique identifier within this DAG.
        std::vector<int> predecessors; ///< IDs of nodes whose output feeds this node.
        std::vector<int> successors;   ///< IDs of nodes that depend on this node's output.
        ComponentBase*   component;    ///< The computation associated with this node.
    };

    DAG() {}

    /**
     * @brief Add a new node to the graph.
     * @param id        Unique integer ID for the node.
     * @param component Pointer to the component. Must outlive the DAG.
     */
    void addNode(int id, ComponentBase* component);

    /**
     * @brief Add a directed edge from node @p from to node @p to.
     *
     * Both nodes must have been added with addNode() before calling this.
     * Calling addEdge() with IDs that do not exist is silently ignored.
     *
     * @param from ID of the upstream (producer) node.
     * @param to   ID of the downstream (consumer) node.
     */
    void addEdge(int from, int to);

    /** @brief Return a copy of the node list (order matches insertion order). */
    std::vector<Node> getNodes() const;

private:
    std::vector<Node> nodes_;
};

#endif // DAG_HPP
