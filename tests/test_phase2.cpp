/**
 * @file test_phase2.cpp
 * @brief Phase 2 tests — ITC, ComponentPort, fan-in, Step 5 drain.
 *
 * Run with: make test_phase2
 * Run with sanitizers: make test_phase2_ubsan  /  make test_phase2_tsan
 */

#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>

#include "../component.hpp"
#include "../demultiplexer.hpp"
#include "../team_manager.hpp"

// ================================================================
//  Helpers
// ================================================================

static int passed = 0;
static int failed = 0;

#define RUN(fn) do { \
    std::cout << "  " #fn " ... "; \
    if (fn()) { std::cout << "PASS\n"; passed++; } \
    else       { std::cout << "FAIL\n"; failed++; } \
} while(0)

static void wait_for(std::atomic<int>& counter, int target, int timeout_ms = 2000) {
    for (int i = 0; i < timeout_ms / 10 && counter.load() < target; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// ================================================================
//  2.1 — ComponentPort tests
// ================================================================

static bool test_port_push_pop() {
    ComponentPort<int, 8> port;
    if (!port.push(42)) return false;
    return port.peek() == 42;
}

static bool test_port_has_data_before_push() {
    ComponentPort<int, 8> port;
    return !port.has_data();
}

static bool test_port_has_data_after_push() {
    ComponentPort<int, 8> port;
    port.push(99);
    return port.has_data();
}

static bool test_port_consume_clears_data() {
    ComponentPort<int, 8> port;
    port.push(7);
    port.peek();
    port.consume();
    return !port.has_data();
}

static bool test_port_overflow() {
    ComponentPort<int, 4> port;
    for (int i = 0; i < 4; i++)
        if (!port.push(i)) return false;
    return !port.push(99);  // 5th push must fail
}

static bool test_port_fifo_order() {
    ComponentPort<int, 8> port;
    for (int i = 0; i < 5; i++) port.push(i * 10);
    for (int i = 0; i < 5; i++) {
        if (port.peek() != i * 10) return false;
        port.consume();
    }
    return true;
}

// 2.1 — ComponentBase::has_pending_input() delegation

struct TestConfig {};

struct TestComponent : public Component<int, int, TestConfig> {
    TestConfig cfg;
    TestComponent() : Component<int, int, TestConfig>(&cfg) {}
    void execute() override { output_ = input_ * 2; }
};

static bool test_component_has_pending_input_false() {
    TestComponent c;
    return !c.has_pending_input();
}

static bool test_component_has_pending_input_true() {
    TestComponent c;
    c.input_port.push(42);
    return c.has_pending_input();
}

static bool test_backward_compat_plain_members() {
    TestComponent c;
    c.init_input(5);
    c.execute();
    return c.output_ == 10;
}

// ================================================================
//  2.0 + 2.2 — TeamManager with Demultiplexer + fan-in
// ================================================================

static bool test_pipeline_simple() {
    // T1 → T2: trigger T1, verify T2 executes
    std::atomic<int> t2_count{0};

    TeamManager tm("test_simple");
    Demultiplexer* d0 = tm.add_demultiplexer(0, 1);
    Demultiplexer* d1 = tm.add_demultiplexer(0, 1);  // same core, same prio — ok for test

    SubtaskDescriptor* t1 = tm.add_subtask("T1", d0, []() {});
    SubtaskDescriptor* t2 = tm.add_subtask("T2", d1, [&]() { t2_count++; });

    tm.add_connection(t1, t2);
    tm.start();

    tm.trigger(t1);
    wait_for(t2_count, 1);

    tm.stop();
    return t2_count.load() == 1;
}

static bool test_single_supplier_unchanged() {
    // With 1 supplier, behaviour is identical to pre-fan-in code.
    std::atomic<int> count{0};

    TeamManager tm("test_single");
    Demultiplexer* d = tm.add_demultiplexer(0, 1);

    SubtaskDescriptor* s = tm.add_subtask("S", d, []() {});
    SubtaskDescriptor* c = tm.add_subtask("C", d, [&]() { count++; });

    tm.add_connection(s, c);
    tm.start();

    tm.trigger(s);
    wait_for(count, 1);

    tm.trigger(s);
    wait_for(count, 2);

    tm.stop();
    return count.load() == 2;
}

static bool test_fanin_waits_all() {
    // 3 suppliers → 1 consumer: consumer must not execute until all 3 notify.
    std::atomic<int> consumer_count{0};

    TeamManager tm("test_fanin");
    Demultiplexer* d = tm.add_demultiplexer(0, 1);

    SubtaskDescriptor* s1 = tm.add_subtask("S1", d, []() {});
    SubtaskDescriptor* s2 = tm.add_subtask("S2", d, []() {});
    SubtaskDescriptor* s3 = tm.add_subtask("S3", d, []() {});
    SubtaskDescriptor* c  = tm.add_subtask("C",  d, [&]() { consumer_count++; });

    tm.add_connection(s1, c);
    tm.add_connection(s2, c);
    tm.add_connection(s3, c);
    tm.start();

    // Trigger only 2 of the 3 suppliers
    tm.trigger(s1);
    tm.trigger(s2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (consumer_count.load() != 0) { tm.stop(); return false; }

    // Now trigger the 3rd — consumer must execute exactly once
    tm.trigger(s3);
    wait_for(consumer_count, 1);

    tm.stop();
    return consumer_count.load() == 1;
}

static bool test_fanin_resets_counter() {
    // Run 2 complete fan-in cycles; counter must reset correctly.
    std::atomic<int> consumer_count{0};

    TeamManager tm("test_fanin_reset");
    Demultiplexer* d = tm.add_demultiplexer(0, 1);

    SubtaskDescriptor* s1 = tm.add_subtask("S1", d, []() {});
    SubtaskDescriptor* s2 = tm.add_subtask("S2", d, []() {});
    SubtaskDescriptor* c  = tm.add_subtask("C",  d, [&]() { consumer_count++; });

    tm.add_connection(s1, c);
    tm.add_connection(s2, c);
    tm.start();

    // First cycle
    tm.trigger(s1);
    tm.trigger(s2);
    wait_for(consumer_count, 1);

    // Second cycle — counter must have reset to 2
    tm.trigger(s1);
    tm.trigger(s2);
    wait_for(consumer_count, 2);

    tm.stop();
    return consumer_count.load() == 2;
}

// ================================================================
//  2.3 — Step 5 drain (has_pending_input)
// ================================================================

static bool test_drain_with_component_port() {
    // Wire has_pending_input to a real ComponentPort.
    // Push 3 values before the subtask can process them.
    // The drain should execute 3 times without extra notifications.

    std::atomic<int> exec_count{0};
    ComponentPort<int, 8> port;

    // Pre-load 3 values
    port.push(1);
    port.push(2);
    port.push(3);

    Demultiplexer demux(0, 1);

    SubtaskDescriptor desc;
    desc.name    = "drain_test";
    desc.execute = [&]() {
        if (port.has_data()) {
            port.consume();
            exec_count++;
        }
    };
    desc.has_pending_input = [&]() { return port.has_data(); };

    int efd = demux.register_subtask(&desc);
    demux.start();

    // Send a single notification — drain should process all 3
    uint64_t sig = 1;
    write(efd, &sig, sizeof(sig));

    // Wait for all 3 to drain
    for (int i = 0; i < 200 && exec_count.load() < 3; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    demux.stop();
    return exec_count.load() == 3;
}

static bool test_drain_stops_when_empty() {
    // Subtask with no has_pending_input: executes exactly once per notification.
    std::atomic<int> exec_count{0};

    Demultiplexer demux(0, 1);
    SubtaskDescriptor desc;
    desc.name    = "no_drain";
    desc.execute = [&]() { exec_count++; };
    // has_pending_input left empty → while loop condition is false

    int efd = demux.register_subtask(&desc);
    demux.start();

    uint64_t sig = 1;
    write(efd, &sig, sizeof(sig));

    for (int i = 0; i < 100 && exec_count.load() < 1; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    demux.stop();

    return exec_count.load() == 1;  // must not loop infinitely
}

// ================================================================
//  2.0 — Release guard active via TeamManager
// ================================================================

static bool test_period_active_via_team_manager() {
    // Set is_periodic + period_ns on a descriptor via set_period().
    // Verify that next_release is properly initialised after start().
    std::atomic<int> exec_count{0};

    TeamManager tm("test_periodic");
    Demultiplexer* d = tm.add_demultiplexer(0, 1);
    SubtaskDescriptor* s = tm.add_subtask("periodic", d, [&]() { exec_count++; });

    tm.set_period(s, 200'000'000L);  // 200ms

    tm.start();

    // next_release must have been initialised to a real monotonic time
    bool not_epoch = (s->next_release.tv_sec > 0);
    bool is_periodic_set = s->is_periodic;

    // Send 3 rapid notifications — only 1 should execute within 50ms
    uint64_t sig = 1;
    write(s->efd, &sig, sizeof(sig));
    write(s->efd, &sig, sizeof(sig));
    write(s->efd, &sig, sizeof(sig));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int count_at_50ms = exec_count.load();

    tm.stop();
    return not_epoch && is_periodic_set && count_at_50ms == 1;
}

// ================================================================
//  main
// ================================================================

int main() {
    std::cout << "\n=== Phase 2 Tests ===\n\n";

    std::cout << "-- 2.1 ComponentPort --\n";
    RUN(test_port_push_pop);
    RUN(test_port_has_data_before_push);
    RUN(test_port_has_data_after_push);
    RUN(test_port_consume_clears_data);
    RUN(test_port_overflow);
    RUN(test_port_fifo_order);
    RUN(test_component_has_pending_input_false);
    RUN(test_component_has_pending_input_true);
    RUN(test_backward_compat_plain_members);

    std::cout << "\n-- 2.0 + 2.2 TeamManager + fan-in --\n";
    RUN(test_pipeline_simple);
    RUN(test_single_supplier_unchanged);
    RUN(test_fanin_waits_all);
    RUN(test_fanin_resets_counter);

    std::cout << "\n-- 2.3 Step 5 drain --\n";
    RUN(test_drain_with_component_port);
    RUN(test_drain_stops_when_empty);

    std::cout << "\n-- 2.0 Release guard via TeamManager --\n";
    RUN(test_period_active_via_team_manager);

    std::cout << "\n=== Results: " << passed << " passed, "
              << failed << " failed ===\n";
    return failed == 0 ? 0 : 1;
}
