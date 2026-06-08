#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include "../dispatcher.hpp"
#include "../demultiplexer.hpp"

// -----------------------------------------------------------------------
//  T1: Chain A→B→C via downstream connections
//      A executes → automatically notifies B → B executes → notifies C
// -----------------------------------------------------------------------
static void test_dispatcher_chain() {
    std::atomic<int> order{0};
    int a_order = -1, b_order = -1, c_order = -1;

    Subtask a(1, [&] { a_order = order.fetch_add(1); });
    Subtask b(2, [&] { b_order = order.fetch_add(1); });
    Subtask c(3, [&] { c_order = order.fetch_add(1); });

    Dispatcher disp(0, 10);
    a.downstream.push_back({&disp, &b});
    b.downstream.push_back({&disp, &c});

    disp.register_subtask(&a);
    disp.register_subtask(&b);
    disp.register_subtask(&c);
    disp.start();

    disp.notify(&a);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    disp.stop();

    assert(a_order == 0);
    assert(b_order == 1);
    assert(c_order == 2);
    std::cout << "[PASS] test_dispatcher_chain\n";
}

// -----------------------------------------------------------------------
//  T2: Fan-in — two dispatchers both downstream to a third subtask.
//      C only runs after both A and B have completed.
// -----------------------------------------------------------------------
static void test_dispatcher_fanin() {
    std::atomic<int> exec_count{0};
    std::atomic<bool> a_done{false}, b_done{false};

    Subtask a(1, [&] { a_done.store(true); });
    Subtask b(2, [&] { b_done.store(true); });
    Subtask c(3, [&] { exec_count.fetch_add(1); });
    c.fan_in_total = 2; // expects two suppliers

    Dispatcher disp_a(0, 10);
    Dispatcher disp_b(0, 9); // same core, different priority object (test only)
    Dispatcher disp_c(0, 8);

    a.downstream.push_back({&disp_c, &c});
    b.downstream.push_back({&disp_c, &c});

    disp_a.register_subtask(&a);
    disp_b.register_subtask(&b);
    disp_c.register_subtask(&c);

    disp_a.start();
    disp_b.start();
    disp_c.start();

    // Notify A only → C should NOT run yet
    disp_a.notify(&a);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(exec_count.load() == 0);

    // Notify B → both suppliers done → C should run
    disp_b.notify(&b);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(exec_count.load() == 1);

    disp_a.stop();
    disp_b.stop();
    disp_c.stop();
    std::cout << "[PASS] test_dispatcher_fanin\n";
}

// -----------------------------------------------------------------------
//  T3: Release-guard — subtask with period=100ms.
//      4 rapid notifications → 4 executions with intervals ≥ 100ms.
// -----------------------------------------------------------------------
static void test_dispatcher_release_guard() {
    constexpr uint64_t PERIOD_NS = 100'000'000ULL; // 100 ms
    constexpr int      JOBS      = 4;

    std::atomic<int> exec_count{0};
    uint64_t timestamps[JOBS] = {};

    Subtask s(1, [&] {
        int idx = exec_count.fetch_add(1);
        if (idx < JOBS) timestamps[idx] = Dispatcher::monotonic_ns();
    });
    s.period_ns = PERIOD_NS;

    Dispatcher disp(0, 10);
    disp.register_subtask(&s);
    disp.start();

    // Fire all 4 notifications at once; release-guard defers 3 of them
    for (int i = 0; i < JOBS; i++) disp.notify(&s);

    // Wait enough for all 4 to fire (4 × 100ms + margin)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    disp.stop();

    assert(exec_count.load() == JOBS);

    for (int i = 1; i < JOBS; i++) {
        uint64_t interval = timestamps[i] - timestamps[i - 1];
        // Allow 20 ms jitter below the nominal period (idle thread polling)
        assert(interval >= PERIOD_NS - 20'000'000ULL);
    }
    std::cout << "[PASS] test_dispatcher_release_guard\n";
}

// -----------------------------------------------------------------------
//  T4: in_processing flag — second rapid notify is dropped if the first
//      subtask is still executing (leader/followers guard).
// -----------------------------------------------------------------------
static void test_dispatcher_in_processing() {
    std::atomic<int> exec_count{0};
    std::atomic<bool> executing{false};
    bool overlap_detected = false;

    Subtask s(1, [&] {
        if (executing.exchange(true)) {
            overlap_detected = true; // concurrent execution detected
        }
        exec_count.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        executing.store(false);
    });

    Dispatcher disp(0, 10);
    disp.register_subtask(&s);
    disp.start();

    disp.notify(&s);
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let it start
    disp.notify(&s); // arrives while first is still running
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    disp.stop();

    assert(!overlap_detected);
    std::cout << "[PASS] test_dispatcher_in_processing\n";
}

// -----------------------------------------------------------------------
//  T5: Aperiodic subtask — simple notify→execute, no timer involvement
// -----------------------------------------------------------------------
static void test_dispatcher_aperiodic() {
    std::atomic<int> count{0};
    Subtask s(1, [&]() -> void { count.fetch_add(1); });

    Dispatcher disp(0, 10);
    disp.register_subtask(&s);
    disp.start();

    disp.notify(&s);
    disp.notify(&s);
    disp.notify(&s);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    disp.stop();

    assert(count.load() == 3);
    std::cout << "[PASS] test_dispatcher_aperiodic\n";
}

// -----------------------------------------------------------------------
//  T6: Demultiplexer::process() delegates correctly to Dispatcher
// -----------------------------------------------------------------------
static void test_demultiplexer_process() {
    std::atomic<int> count{0};
    Subtask s(1, [&] { count.fetch_add(1); });

    Dispatcher disp(0, 10);
    disp.start();

    // Call process() directly (bypasses FIFO queue — tests the 6-step logic)
    Demultiplexer::process(&s, disp);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    disp.stop();

    assert(count.load() == 1);
    std::cout << "[PASS] test_demultiplexer_process\n";
}

int main() {
    test_dispatcher_chain();
    test_dispatcher_fanin();
    test_dispatcher_release_guard();
    test_dispatcher_in_processing();
    test_dispatcher_aperiodic();
    test_demultiplexer_process();
    std::cout << "\nTodos os testes da Fase 4 passaram.\n";
    return 0;
}
