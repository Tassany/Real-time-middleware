#include <cassert>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include "dag.hpp"

// Chain 1→2→3→4
static void test_dag_linear() {
    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_node(4, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(2, 3);
    dag.add_edge(3, 4);

    auto sorted = dag.topological_sort();
    assert(sorted.size() == 4);
    assert(sorted[0] == 1);
    assert(sorted[1] == 2);
    assert(sorted[2] == 3);
    assert(sorted[3] == 4);
    assert(dag.pipeline_depth() == 4);
    assert(!dag.has_cycle());

    std::cout << "[PASS] test_dag_linear\n";
}

// Cycle 1→2→1
static void test_dag_cycle() {
    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(2, 1);

    assert(dag.has_cycle());

    bool threw = false;
    try { dag.topological_sort(); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    std::cout << "[PASS] test_dag_cycle\n";
}

// Fan-in: 1→3, 2→3
static void test_dag_fan_in() {
    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_edge(1, 3);
    dag.add_edge(2, 3);

    assert(dag.fan_in_count(3)  == 2);
    assert(dag.fan_out_count(1) == 1);
    assert(dag.fan_out_count(2) == 1);
    assert(dag.fan_in_count(1)  == 0);
    assert(!dag.has_cycle());

    std::cout << "[PASS] test_dag_fan_in\n";
}

// Diamond: 1→2, 1→3, 2→4, 3→4
static void test_dag_diamond() {
    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_node(4, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(1, 3);
    dag.add_edge(2, 4);
    dag.add_edge(3, 4);

    assert(!dag.has_cycle());
    assert(dag.pipeline_depth() == 3);

    auto sorted = dag.topological_sort();
    assert(sorted.size() == 4);
    assert(sorted[0] == 1);
    assert(sorted[3] == 4);
    // 2 e 3 devem aparecer antes de 4
    auto pos = [&](int id) {
        return std::find(sorted.begin(), sorted.end(), id) - sorted.begin();
    };
    assert(pos(2) < pos(4));
    assert(pos(3) < pos(4));

    std::cout << "[PASS] test_dag_diamond\n";
}

// Single node with non-zero ID
static void test_dag_single_node() {
    DAG dag;
    dag.add_node(42, nullptr);

    auto sorted = dag.topological_sort();
    assert(sorted.size() == 1);
    assert(sorted[0] == 42);
    assert(dag.pipeline_depth() == 1);
    assert(!dag.has_cycle());
    assert(dag.fan_in_count(42)  == 0);
    assert(dag.fan_out_count(42) == 0);

    std::cout << "[PASS] test_dag_single_node\n";
}

// Node not found → fan_in/fan_out return -1
static void test_dag_node_not_found() {
    DAG dag;
    dag.add_node(1, nullptr);

    assert(dag.fan_in_count(99)  == -1);
    assert(dag.fan_out_count(99) == -1);

    std::cout << "[PASS] test_dag_node_not_found\n";
}

// pipeline_depth on cycle must throw
static void test_dag_depth_throws_on_cycle() {
    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(2, 1);

    bool threw = false;
    try { dag.pipeline_depth(); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    std::cout << "[PASS] test_dag_depth_throws_on_cycle\n";
}

int main() {
    test_dag_linear();
    test_dag_cycle();
    test_dag_fan_in();
    test_dag_diamond();
    test_dag_single_node();
    test_dag_node_not_found();
    test_dag_depth_throws_on_cycle();
    std::cout << "\nPhase 1 - completed. \n";
    return 0;
}
