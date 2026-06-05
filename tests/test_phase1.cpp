/**
 * @file test_phase1.cpp
 * @brief Phase 1 regression tests — no external framework required.
 *
 * Each test is a function that returns true on pass, false on fail.
 * Run with: make test_phase1
 * Run with sanitizers: make test_phase1_ubsan  or  make test_phase1_tsan
 */

#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <fstream>

#include "../ring_buffer.hpp"
#include "../demultiplexer.hpp"
#include "../parser_json.hpp"

// ================================================================
//  Test helpers
// ================================================================

static int passed = 0;
static int failed = 0;

#define RUN(fn) do { \
    std::cout << "  " #fn " ... "; \
    if (fn()) { std::cout << "PASS\n"; passed++; } \
    else       { std::cout << "FAIL\n"; failed++; } \
} while(0)

// ================================================================
//  1.3 — RingBuffer tests
// ================================================================

static bool test_ringbuffer_write_read() {
    RingBuffer<int, 8> rb;
    for (size_t i = 0; i < 7; i++) {
        if (!rb.write(i, static_cast<int>(i * 10))) return false;
    }
    for (size_t i = 0; i < 7; i++) {
        if (rb.read(i) != static_cast<int>(i * 10)) return false;
    }
    return true;
}

static bool test_ringbuffer_overflow() {
    RingBuffer<int, 4> rb;
    // Fill all 4 slots without releasing
    for (size_t i = 0; i < 4; i++) {
        if (!rb.write(i, 0)) return false;
    }
    // 5th write must fail (buffer full)
    if (rb.write(4, 0)) return false;
    return true;
}

static bool test_ringbuffer_release_allows_write() {
    RingBuffer<int, 4> rb;
    // Fill buffer
    for (size_t i = 0; i < 4; i++) rb.write(i, static_cast<int>(i));
    // Next write fails
    if (rb.write(4, 99)) return false;
    // Release slot 0
    rb.release(0);
    // Now seq 4 should succeed (slot 0 is free)
    if (!rb.write(4, 99)) return false;
    if (rb.read(4) != 99) return false;
    return true;
}

static bool test_ringbuffer_has_data() {
    RingBuffer<int, 4> rb;
    if (rb.has_data(0)) return false;   // nothing written yet
    rb.write(0, 42);
    if (!rb.has_data(0)) return false;  // seq 0 was written
    if (rb.has_data(1))  return false;  // seq 1 not written yet
    return true;
}

// Concurrent: 1 producer / 1 consumer, 1000 iterations
static bool test_ringbuffer_concurrent() {
    RingBuffer<int, 16> rb;
    std::atomic<bool> done{false};
    std::atomic<int>  errors{0};

    std::thread producer([&]() {
        for (size_t i = 0; i < 1000; i++) {
            while (!rb.write(i, static_cast<int>(i))) { /* spin */ }
        }
        done = true;
    });

    std::thread consumer([&]() {
        for (size_t i = 0; i < 1000; i++) {
            // Wait until data is available
            while (!rb.has_data(i)) { /* spin */ }
            if (rb.read(i) != static_cast<int>(i)) errors++;
            rb.release(i);
        }
    });

    producer.join();
    consumer.join();
    return errors.load() == 0;
}

// ================================================================
//  1.1 — Demultiplexer epoll union tests
// ================================================================

static bool test_demux_stop() {
    Demultiplexer demux(0, 1);
    demux.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    demux.stop();
    // If we reach here without deadlock or crash, the test passes.
    return true;
}

static bool test_demux_subtask_event() {
    Demultiplexer demux(0, 1);

    std::atomic<int> call_count{0};

    SubtaskDescriptor desc;
    desc.name    = "test_subtask";
    desc.execute = [&]() { call_count++; };

    int efd = demux.register_subtask(&desc);

    demux.start();

    // Signal the subtask
    uint64_t sig = 1;
    write(efd, &sig, sizeof(sig));

    // Wait up to 500ms for the subtask to be called
    for (int i = 0; i < 50 && call_count.load() == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    demux.stop();
    return call_count.load() == 1;
}

static bool test_demux_multiple_events() {
    Demultiplexer demux(0, 1);

    std::atomic<int> call_count{0};

    SubtaskDescriptor desc;
    desc.name    = "multi";
    desc.execute = [&]() { call_count++; };

    int efd = demux.register_subtask(&desc);
    demux.start();

    // Send 3 signals
    uint64_t sig = 1;
    write(efd, &sig, sizeof(sig));
    write(efd, &sig, sizeof(sig));
    write(efd, &sig, sizeof(sig));

    // Wait for all 3 to be processed
    for (int i = 0; i < 100 && call_count.load() < 3; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    demux.stop();
    return call_count.load() == 3;
}

// ================================================================
//  1.2 — next_release initialisation tests
// ================================================================

static bool test_release_guard_no_past_release() {
    // After start(), a periodic subtask's next_release must be >= the
    // current time (not stuck at Unix epoch {0,0}).
    Demultiplexer demux(0, 1);

    SubtaskDescriptor desc;
    desc.name        = "periodic";
    desc.execute     = []() {};
    desc.is_periodic = true;
    desc.period_ns   = 100'000'000L;  // 100ms

    demux.register_subtask(&desc);

    timespec before;
    clock_gettime(CLOCK_MONOTONIC, &before);

    demux.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    demux.stop();

    // next_release must be >= before (not the epoch)
    bool not_epoch = (desc.next_release.tv_sec > 0);
    bool after_before = (desc.next_release.tv_sec > before.tv_sec) ||
                        (desc.next_release.tv_sec == before.tv_sec &&
                         desc.next_release.tv_nsec >= before.tv_nsec);

    return not_epoch && after_before;
}

static bool test_release_guard_period_respected() {
    // A periodic subtask sent 3 rapid notifications should not execute
    // more than once within the first period.
    Demultiplexer demux(0, 1);

    std::atomic<int> call_count{0};

    SubtaskDescriptor desc;
    desc.name        = "guarded";
    desc.execute     = [&]() { call_count++; };
    desc.is_periodic = true;
    desc.period_ns   = 200'000'000L;  // 200ms

    int efd = demux.register_subtask(&desc);
    demux.start();

    // Rapid-fire 3 notifications
    uint64_t sig = 1;
    write(efd, &sig, sizeof(sig));
    write(efd, &sig, sizeof(sig));
    write(efd, &sig, sizeof(sig));

    // Wait 50ms — well within the 200ms period
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int count_before = call_count.load();

    demux.stop();

    // Should have executed exactly once (the other two are in the timer queue)
    return count_before == 1;
}

// ================================================================
//  1.4 — Parser optional fields tests
// ================================================================

static bool test_parser_with_period() {
    // Write a minimal JSON to a temp file
    const char* tmpfile = "/tmp/test_plan_with_period.json";
    {
        std::ofstream f(tmpfile);
        f << R"({
            "hosts": [{ "name": "h1", "address": "127.0.0.1" }],
            "tasks": [{
                "name": "t1",
                "subtasks": [{
                    "name": "S1", "component": "C1", "host": "h1",
                    "core": 0, "priority": 50,
                    "period_ns": 5000000, "deadline_ns": 5000000
                }]
            }],
            "connections": []
        })";
    }
    try {
        JsonParser parser;
        DeploymentPlan plan = parser.parse(tmpfile);
        return plan.tasks[0].subtasks[0].period_ns == 5000000L &&
               plan.tasks[0].subtasks[0].deadline_ns == 5000000L;
    } catch (...) { return false; }
}

static bool test_parser_without_period() {
    // JSON without period_ns/deadline_ns — must not throw, default to 0
    const char* tmpfile = "/tmp/test_plan_no_period.json";
    {
        std::ofstream f(tmpfile);
        f << R"({
            "hosts": [{ "name": "h1", "address": "127.0.0.1" }],
            "tasks": [{
                "name": "t1",
                "subtasks": [{
                    "name": "S1", "component": "C1", "host": "h1",
                    "core": 0, "priority": 50
                }]
            }],
            "connections": []
        })";
    }
    try {
        JsonParser parser;
        DeploymentPlan plan = parser.parse(tmpfile);
        return plan.tasks[0].subtasks[0].period_ns   == 0L &&
               plan.tasks[0].subtasks[0].deadline_ns == 0L;
    } catch (...) { return false; }
}

// ================================================================
//  main
// ================================================================

int main() {
    std::cout << "\n=== Phase 1 Tests ===\n\n";

    std::cout << "-- 1.3 RingBuffer --\n";
    RUN(test_ringbuffer_write_read);
    RUN(test_ringbuffer_overflow);
    RUN(test_ringbuffer_release_allows_write);
    RUN(test_ringbuffer_has_data);
    RUN(test_ringbuffer_concurrent);

    std::cout << "\n-- 1.1 Demultiplexer epoll union --\n";
    RUN(test_demux_stop);
    RUN(test_demux_subtask_event);
    RUN(test_demux_multiple_events);

    std::cout << "\n-- 1.2 next_release initialisation --\n";
    RUN(test_release_guard_no_past_release);
    RUN(test_release_guard_period_respected);

    std::cout << "\n-- 1.4 Parser optional fields --\n";
    RUN(test_parser_with_period);
    RUN(test_parser_without_period);

    std::cout << "\n=== Results: " << passed << " passed, " << failed << " failed ===\n";
    return failed == 0 ? 0 : 1;
}
