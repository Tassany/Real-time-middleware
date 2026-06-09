/**
 * example_team_manager.cpp
 *
 * Demonstrates TeamManager wiring two independent pipelines from a DAG,
 * without any manual downstream or fan_in_total setup.
 *
 *  Pipeline A — linear chain, periodic (400 ms):
 *
 *    [Sensor(1)] ──► [Filter(2)] ──► [Display(3)]
 *
 *    TeamManager sets: Filter.fan_in_total=1, Display.fan_in_total=1.
 *    Main thread calls tm.notify(1) every ~350 ms; release-guard ensures
 *    the effective interval is never shorter than 400 ms.
 *
 *  Pipeline B — fan-in merge (aperiodic):
 *
 *    [Accel(4)] ──┐
 *                  ├──► [IMU Fusion(6)]
 *    [Gyro (5)] ──┘
 *
 *    TeamManager sets: Fusion.fan_in_total=2.
 *    Main thread calls tm.notify(4) and tm.notify(5) independently;
 *    Fusion only fires when both have been received for the same job.
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "team_manager.hpp"

// -----------------------------------------------------------------------
//  Shared data between stages (plain atomics for this illustration)
// -----------------------------------------------------------------------
static std::atomic<double> raw_temp{20.0};
static std::atomic<double> filtered_temp{20.0};

static std::atomic<double> accel_x{0.0};
static std::atomic<double> gyro_z{0.0};

// -----------------------------------------------------------------------
//  Helper: build SubtaskInfo with the minimum fields TeamManager needs
// -----------------------------------------------------------------------
static SubtaskInfo make_info(int id, int core, int priority,
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

int main() {
    // -------------------------------------------------------------------
    //  Define subtask logic
    // -------------------------------------------------------------------

    // --- Pipeline A ---
    Subtask sensor(1, [] {
        raw_temp.store(raw_temp.load() + 0.3);
        std::cout << "[Sensor ] raw = " << raw_temp.load() << " °C\n";
    });

    Subtask filter(2, [] {
        double v = raw_temp.load() * 0.9 + filtered_temp.load() * 0.1;
        filtered_temp.store(v);
        std::cout << "[Filter ] filtered = " << filtered_temp.load() << " °C\n";
    });

    Subtask display(3, [] {
        std::cout << "[Display] → " << filtered_temp.load() << " °C  ✓\n";
    });

    // --- Pipeline B ---
    Subtask accel(4, [] {
        accel_x.store(accel_x.load() + 0.1);
        std::cout << "[Accel  ] ax = " << accel_x.load() << " m/s²\n";
    });

    Subtask gyro(5, [] {
        gyro_z.store(gyro_z.load() + 0.5);
        std::cout << "[Gyro   ] gz = " << gyro_z.load() << " °/s\n";
    });

    Subtask imu_fusion(6, [] {
        std::cout << "[Fusion ] ax=" << accel_x.load()
                  << "  gz=" << gyro_z.load()
                  << "  ← both suppliers ready\n";
    });

    // -------------------------------------------------------------------
    //  Build the combined DAG
    // -------------------------------------------------------------------
    DAG dag;
    dag.add_node(1, nullptr);  // Sensor
    dag.add_node(2, nullptr);  // Filter
    dag.add_node(3, nullptr);  // Display
    dag.add_node(4, nullptr);  // Accel
    dag.add_node(5, nullptr);  // Gyro
    dag.add_node(6, nullptr);  // IMU Fusion

    // Pipeline A: 1 → 2 → 3
    dag.add_edge(1, 2);
    dag.add_edge(2, 3);

    // Pipeline B: 4 → 6,  5 → 6
    dag.add_edge(4, 6);
    dag.add_edge(5, 6);

    // -------------------------------------------------------------------
    //  Initialize TeamManager — all wiring is derived from the DAG above.
    //  No manual downstream.push_back() or fan_in_total needed.
    // -------------------------------------------------------------------
    constexpr uint64_t PERIOD_A   = 400'000'000ULL;   // 400 ms
    constexpr uint64_t DEADLINE_A = 400'000'000ULL;   // same as period

    TeamManager tm;
    tm.initialize({
        {make_info(1, /*core*/0, /*prio*/10, PERIOD_A,   DEADLINE_A), &sensor},
        {make_info(2, 0, 9,                  PERIOD_A,   DEADLINE_A), &filter},
        {make_info(3, 0, 8,                  PERIOD_A,   DEADLINE_A), &display},
        {make_info(4, 0, 7,                  PERIOD_A,   DEADLINE_A), &accel},
        {make_info(5, 0, 6,                  PERIOD_A,   DEADLINE_A), &gyro},
        {make_info(6, 0, 5,                  PERIOD_A,   DEADLINE_A), &imu_fusion},
    }, dag);

    // Print recommended ring buffer sizes computed from DAG + SubtaskInfo.
    // These values should be used as the compile-time N in RingBuffer<T, N>.
    //   N = max(2, ceil(deadline_downstream / period_upstream) + pipeline_depth)
    std::cout << "Ring buffer sizes (N):\n";
    std::cout << "  edge 1→2: " << tm.ring_buffer_size(1, 2) << "\n";
    std::cout << "  edge 2→3: " << tm.ring_buffer_size(2, 3) << "\n";
    std::cout << "  edge 4→6: " << tm.ring_buffer_size(4, 6) << "\n";
    std::cout << "  edge 5→6: " << tm.ring_buffer_size(5, 6) << "\n\n";

    tm.start();

    // -------------------------------------------------------------------
    //  Drive Pipeline A: 4 activations, 350 ms apart.
    //  Sensor period is 400 ms → release-guard defers early arrivals.
    // -------------------------------------------------------------------
    std::cout << "\n=== Pipeline A: periodic chain (400 ms period) ===\n";
    for (int i = 0; i < 4; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        tm.notify(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // -------------------------------------------------------------------
    //  Drive Pipeline B: fan-in gate.
    // -------------------------------------------------------------------
    std::cout << "\n=== Pipeline B: fan-in (Fusion fires when both sensors ready) ===\n";

    // Job 1: both sensors notify → Fusion fires
    std::cout << "-- notify Accel + Gyro (job 1) --\n";
    tm.notify(4);
    tm.notify(5);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Job 2: only Accel → Fusion stays silent (counter=1)
    std::cout << "-- notify Accel only (job 2) --\n";
    tm.notify(4);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Job 3: Gyro completes the pair → Fusion fires
    std::cout << "-- notify Gyro only (job 3, completes job 2 pair) --\n";
    tm.notify(5);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // -------------------------------------------------------------------
    //  Shutdown
    // -------------------------------------------------------------------
    tm.stop();
    std::cout << "\nDone. Final state: TERMINATED\n";
    return 0;
}
