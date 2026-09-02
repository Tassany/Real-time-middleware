#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include "team_manager.hpp"

// Helper: build a minimal SubtaskInfo
static SubtaskInfo make_info(int id, int core = 0, int priority = 10,
                              uint64_t period_ns = 0, uint64_t deadline_ns = 0) {
    SubtaskInfo info;
    info.id             = id;
    info.core           = core;
    info.priority       = priority;
    info.period_ns      = period_ns;
    info.deadline_ns    = deadline_ns;
    info.component_type = "";
    info.host           = "localhost";
    return info;
}

// -----------------------------------------------------------------------
//  T1: State transitions CREATED→INITIALIZED→RUNNING→TERMINATED
// -----------------------------------------------------------------------
static void test_team_lifecycle() {
    Subtask s(1, [] {});
    DAG dag;
    dag.add_node(1, nullptr);

    TeamManager tm;
    assert(tm.state() == TeamManager::State::CREATED);

    tm.initialize({{make_info(1), &s}}, dag);
    assert(tm.state() == TeamManager::State::INITIALIZED);

    tm.start();
    assert(tm.state() == TeamManager::State::RUNNING);

    tm.stop();
    assert(tm.state() == TeamManager::State::TERMINATED);

    std::cout << "[PASS] test_team_lifecycle\n";
}

// -----------------------------------------------------------------------
//  T2: Auto-wiring — linear chain 1→2→3.
//      Only subtask 1 is notified externally; 2 and 3 execute automatically.
// -----------------------------------------------------------------------
static void test_team_auto_wiring() {
    std::atomic<int> order{0};
    int o1 = -1, o2 = -1, o3 = -1;

    Subtask s1(1, [&] { o1 = order.fetch_add(1); });
    Subtask s2(2, [&] { o2 = order.fetch_add(1); });
    Subtask s3(3, [&] { o3 = order.fetch_add(1); });

    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(2, 3);

    TeamManager tm;
    tm.initialize({{make_info(1), &s1},
                   {make_info(2), &s2},
                   {make_info(3), &s3}}, dag);
    tm.start();

    tm.notify(1);   // fire source; 2 and 3 should follow via downstream

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tm.stop();

    assert(o1 == 0);
    assert(o2 == 1);
    assert(o3 == 2);
    std::cout << "[PASS] test_team_auto_wiring\n";
}

// -----------------------------------------------------------------------
//  T3: Auto fan-in — topology 1→3, 2→3.
//      Subtask 3 must wait for both 1 and 2 before executing.
// -----------------------------------------------------------------------
static void test_team_auto_fanin() {
    std::atomic<int> count{0};

    Subtask s1(1, [] {});
    Subtask s2(2, [] {});
    Subtask s3(3, [&] { count.fetch_add(1); });

    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_edge(1, 3);
    dag.add_edge(2, 3);

    TeamManager tm;
    tm.initialize({{make_info(1), &s1},
                   {make_info(2), &s2},
                   {make_info(3), &s3}}, dag);
    tm.start();

    // fan_in_total for s3 should have been set to 2 automatically
    assert(s3.fan_in_total == 2);

    // Only s1 fires → s3 should NOT execute yet
    tm.notify(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(count.load() == 0);

    // s2 fires → fan-in complete → s3 executes
    tm.notify(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(count.load() == 1);

    tm.stop();
    std::cout << "[PASS] test_team_auto_fanin\n";
}

// -----------------------------------------------------------------------
//  T4: Exception handling — a throwing subtask transitions the team to
//      TERMINATING; explicit stop() brings it to TERMINATED.
// -----------------------------------------------------------------------
static void test_team_exception() {
    Subtask s(1, [] { throw std::runtime_error("injected fault"); });

    DAG dag;
    dag.add_node(1, nullptr);

    TeamManager tm;
    tm.initialize({{make_info(1), &s}}, dag);
    tm.start();

    tm.notify(1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(tm.state() == TeamManager::State::TERMINATING);

    // Main thread finalises cleanup (calling stop() from dispatcher thread
    // would deadlock on pthread_join — by design, stop() is for the main thread)
    tm.stop();
    assert(tm.state() == TeamManager::State::TERMINATED);

    std::cout << "[PASS] test_team_exception\n";
}

// -----------------------------------------------------------------------
//  T5: Double stop — idempotent, no crash, state stays TERMINATED
// -----------------------------------------------------------------------
static void test_team_double_stop() {
    Subtask s(1, [] {});
    DAG dag;
    dag.add_node(1, nullptr);

    TeamManager tm;
    tm.initialize({{make_info(1), &s}}, dag);
    tm.start();
    tm.stop();
    tm.stop();   // second call must be a no-op

    assert(tm.state() == TeamManager::State::TERMINATED);
    std::cout << "[PASS] test_team_double_stop\n";
}

// -----------------------------------------------------------------------
//  T6: Ring buffer sizing — N = max(2, ceil(D/T) + pipeline_depth)
// -----------------------------------------------------------------------
static void test_team_ring_buffer_sizing() {
    // Linear chain 1→2→3, depth = 3
    // Subtask 1: period=100ms, deadline=200ms
    // Subtask 2: period=100ms, deadline=200ms
    // Subtask 3: period=100ms, deadline=200ms
    //
    // Edge 1→2: ceil(200ms/100ms) + 3 = 5 → next_pow2(5) = 8
    // Edge 2→3: same → 8

    constexpr uint64_t P = 100'000'000ULL;  // 100 ms
    constexpr uint64_t D = 200'000'000ULL;  // 200 ms

    Subtask s1(1, [] {}), s2(2, [] {}), s3(3, [] {});

    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(2, 3);

    TeamManager tm;
    tm.initialize({
        {make_info(1, 0, 10, P, D), &s1},
        {make_info(2, 0,  9, P, D), &s2},
        {make_info(3, 0,  8, P, D), &s3},
    }, dag);

    // pipeline_depth = 3; ceil(D/P) = 2; base = 5; next_pow2(5) = 8
    assert(tm.ring_buffer_size(1, 2) == 8);
    assert(tm.ring_buffer_size(2, 3) == 8);

    // Non-existent edge returns 0
    assert(tm.ring_buffer_size(1, 3) == 0);

    // Aperiodic upstream (period=0): N = depth + 2 = 3 + 2 = 5
    Subtask a(4, [] {}), b(5, [] {});
    DAG dag2;
    dag2.add_node(4, nullptr);
    dag2.add_node(5, nullptr);
    dag2.add_edge(4, 5);

    TeamManager tm2;
    tm2.initialize({
        {make_info(4, 0, 10, /*period=*/0, D), &a},
        {make_info(5, 0,  9, P,           D), &b},
    }, dag2);

    // depth=2 (two nodes), aperiodic: base = 2+2 = 4; next_pow2(4) = 4
    assert(tm2.ring_buffer_size(4, 5) == 4);

    // Floor: even if ceil(D/P)+depth < 2, result is at least 2
    Subtask x(6, [] {}), y(7, [] {});
    DAG dag3;
    dag3.add_node(6, nullptr);
    dag3.add_node(7, nullptr);
    dag3.add_edge(6, 7);

    TeamManager tm3;
    tm3.initialize({
        {make_info(6, 0, 10, /*period=*/1'000'000'000ULL, /*deadline=*/1ULL), &x},
        {make_info(7, 0,  9, P,                            /*deadline=*/1ULL), &y},
    }, dag3);

    // ceil(1ns / 1s) = 1; depth=2; base = max(2, 1+2) = 3; next_pow2(3) = 4
    assert(tm3.ring_buffer_size(6, 7) == 4);

    std::cout << "[PASS] test_team_ring_buffer_sizing\n";
}

// -----------------------------------------------------------------------
//  T7: Dispatcher sharing — subtasks with the same (core, priority)
//      must share a single Dispatcher, not get one each.
// -----------------------------------------------------------------------
static void test_team_dispatcher_sharing() {
    // 3 subtasks, all core=0 priority=10 → should produce 1 Dispatcher
    Subtask s1(1, [] {}), s2(2, [] {}), s3(3, [] {});

    DAG dag;
    dag.add_node(1, nullptr);
    dag.add_node(2, nullptr);
    dag.add_node(3, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(2, 3);

    TeamManager tm;
    tm.initialize({
        {make_info(1, 0, 10), &s1},
        {make_info(2, 0, 10), &s2},   // same core+priority as s1
        {make_info(3, 0, 10), &s3},   // same core+priority as s1
    }, dag);

    assert(tm.dispatcher_count() == 1);  // one thread for all three

    // 2 subtasks on core=0 prio=10, one on core=0 prio=5 → 2 Dispatchers
    Subtask a(4, [] {}), b(5, [] {}), c(6, [] {});
    DAG dag2;
    dag2.add_node(4, nullptr);
    dag2.add_node(5, nullptr);
    dag2.add_node(6, nullptr);
    dag2.add_edge(4, 6);
    dag2.add_edge(5, 6);

    TeamManager tm2;
    tm2.initialize({
        {make_info(4, 0, 10), &a},
        {make_info(5, 0, 10), &b},   // same as a → shared
        {make_info(6, 0,  5), &c},   // different priority → separate
    }, dag2);

    assert(tm2.dispatcher_count() == 2);

    std::cout << "[PASS] test_team_dispatcher_sharing\n";
}

int main() {
    test_team_lifecycle();
    test_team_auto_wiring();
    test_team_auto_fanin();
    test_team_exception();
    test_team_double_stop();
    test_team_ring_buffer_sizing();
    test_team_dispatcher_sharing();
    std::cout << "\nTodos os testes da Fase 5 passaram.\n";
    return 0;
}
