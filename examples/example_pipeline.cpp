/**
 * example_pipeline.cpp
 *
 * Demonstrates the MCFlow Dispatcher with two independent patterns running
 * simultaneously:
 *
 *  Pattern 1 — Periodic linear chain (500 ms period):
 *
 *    [Sensor] ──downstream──► [Filter] ──downstream──► [Actuator]
 *
 *    - Sensor is triggered every 500 ms by the main thread.
 *    - Release-guard ensures the interval between Sensor jobs is never
 *      shorter than 500 ms even if notify() is called early.
 *    - After Sensor executes, Dispatcher automatically notifies Filter;
 *      after Filter executes it notifies Actuator — no manual wiring
 *      inside execute().
 *
 *  Pattern 2 — Fan-in merge (aperiodic):
 *
 *    [SourceA] ──►
 *                  [Merge]   (fan_in_total = 2)
 *    [SourceB] ──►
 *
 *    - Merge only executes after BOTH SourceA and SourceB have notified
 *      for the same job.
 *    - Fired 3 times: first SourceA alone (Merge silent), then SourceB
 *      alone (Merge silent), then both together (Merge executes).
 *
 * Run without root — SCHED_FIFO requires CAP_SYS_NICE; the dispatcher
 * prints a warning and falls back to the default scheduler, which is fine
 * for demonstration purposes.
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include "dispatcher.hpp"

// -----------------------------------------------------------------------
//  Shared data between pipeline stages (replaced by RingBuffer in
//  production; plain atomics suffice for this illustration).
// -----------------------------------------------------------------------
static std::atomic<double> sensor_value{0.0};
static std::atomic<double> filtered_value{0.0};

static std::atomic<int> source_a_value{0};
static std::atomic<int> source_b_value{0};

// -----------------------------------------------------------------------
//  main
// -----------------------------------------------------------------------
int main() {
    // -------------------------------------------------------------------
    //  Pattern 1: linear chain  Sensor → Filter → Actuator
    //  All three subtasks share one Dispatcher (same core, same priority).
    // -------------------------------------------------------------------
    Dispatcher disp_chain(/*core=*/0, /*priority=*/10);

    Subtask sensor(1, [&] {
        sensor_value.store(sensor_value.load() + 1.5);
        std::cout << "[Sensor  ] raw = " << sensor_value.load() << "\n";
    });
    sensor.period_ns = 500'000'000ULL;   // 500 ms

    Subtask filter(2, [&] {
        double v = sensor_value.load() * 0.8;  // simple gain
        filtered_value.store(v);
        std::cout << "[Filter  ] filtered = " << filtered_value.load() << "\n";
    });

    Subtask actuator(3, [&] {
        std::cout << "[Actuator] output   = " << filtered_value.load()
                  << "  ✓\n";
    });

    // Wire the downstream chain: Dispatcher propagates automatically.
    sensor.downstream.push_back({&disp_chain, &filter});
    filter.downstream.push_back({&disp_chain, &actuator});

    disp_chain.register_subtask(&sensor);
    disp_chain.register_subtask(&filter);
    disp_chain.register_subtask(&actuator);

    // -------------------------------------------------------------------
    //  Pattern 2: fan-in  SourceA + SourceB → Merge
    //  Each source runs on its own Dispatcher; Merge on a third.
    // -------------------------------------------------------------------
    Dispatcher disp_a(/*core=*/0, /*priority=*/9);
    Dispatcher disp_b(/*core=*/0, /*priority=*/8);
    Dispatcher disp_merge(/*core=*/0, /*priority=*/7);

    Subtask source_a(4, [&] {
        source_a_value.fetch_add(10);
        std::cout << "[SourceA ] value = " << source_a_value.load() << "\n";
    });

    Subtask source_b(5, [&] {
        source_b_value.fetch_add(1);
        std::cout << "[SourceB ] value = " << source_b_value.load() << "\n";
    });

    Subtask merge(6, [&] {
        int sum = source_a_value.load() + source_b_value.load();
        std::cout << "[Merge   ] A+B = " << sum << "  ← both suppliers ready\n";
    });
    merge.fan_in_total = 2;   // waits for SourceA AND SourceB

    source_a.downstream.push_back({&disp_merge, &merge});
    source_b.downstream.push_back({&disp_merge, &merge});

    disp_a.register_subtask(&source_a);
    disp_b.register_subtask(&source_b);
    disp_merge.register_subtask(&merge);

    // -------------------------------------------------------------------
    //  Start all dispatchers
    // -------------------------------------------------------------------
    disp_chain.start();
    disp_a.start();
    disp_b.start();
    disp_merge.start();

    // -------------------------------------------------------------------
    //  Drive Pattern 1: 4 periodic activations, 400 ms apart.
    //  The 400 ms interval is shorter than the 500 ms period — release-guard
    //  defers the early jobs to honour the declared period.
    // -------------------------------------------------------------------
    std::cout << "\n=== Pattern 1: periodic chain (500 ms period) ===\n";
    for (int i = 0; i < 4; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        disp_chain.notify(&sensor);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // -------------------------------------------------------------------
    //  Drive Pattern 2: demonstrate fan-in gate.
    // -------------------------------------------------------------------
    std::cout << "\n=== Pattern 2: fan-in (Merge fires only when both ready) ===\n";

    // Job 1: both notify → Merge fires
    std::cout << "-- job 1: notify SourceA and SourceB → Merge fires --\n";
    disp_a.notify(&source_a);
    disp_b.notify(&source_b);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Job 2: only SourceA notifies → counter=1, Merge stays silent
    std::cout << "-- job 2: notify SourceA only → Merge silent --\n";
    disp_a.notify(&source_a);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Job 3: both notify → counter goes 1→2, Merge fires
    std::cout << "-- job 3: notify SourceA and SourceB → Merge fires --\n";
    disp_a.notify(&source_a);
    disp_b.notify(&source_b);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // -------------------------------------------------------------------
    //  Shutdown
    // -------------------------------------------------------------------
    disp_chain.stop();
    disp_a.stop();
    disp_b.stop();
    disp_merge.stop();

    std::cout << "\nDone.\n";
    return 0;
}
