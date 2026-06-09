/**
 * example_full_pipeline.cpp
 *
 * Demonstrates a 6-task, 3-subtask-per-task pipeline using a single
 * TeamManager and a single DAG with 6 disconnected linear chains.
 *
 * Topology (6 independent tasks, each: source → intermediate → sink):
 *
 *   Task 1 (T=1ms,  prio=16):  [S1]→[I2]→[K3]   core 0 → 1 → 2
 *   Task 2 (T=2ms,  prio=14):  [S4]→[I5]→[K6]   core 0 → 1 → 2
 *   Task 3 (T=6ms,  prio=12):  [S7]→[I8]→[K9]   core 0 → 1 → 2
 *   Task 4 (T=12ms, prio=10):  [S10]→[I11]→[K12] core 0 → 1 → 2
 *   Task 5 (T=24ms, prio=8):   [S13]→[I14]→[K15] core 0 → 1 → 2
 *   Task 6 (T=48ms, prio=6):   [S16]→[I17]→[K18] core 0 → 1 → 2
 *
 * Scheduling: RMS (Rate-Monotonic) — shorter period = higher priority.
 * Core 0: all sources.  Core 1: all intermediates.  Core 2: all sinks.
 *
 * The main thread fires sources in a 1 ms tick loop for 4 × LCM = 192 ms:
 *   - Task 1 fires every tick  (1ms)
 *   - Task 2 fires every 2nd   (2ms)
 *   - Task 3 fires every 6th   (6ms)
 *   - Task 4 fires every 12th  (12ms)
 *   - Task 5 fires every 24th  (24ms)
 *   - Task 6 fires every 48th  (48ms)
 *
 * Each pipeline stage carries one double value:
 *   Source      — generates an incrementing counter (job_id)
 *   Intermediate — doubles the value
 *   Sink        — prints the result
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <iomanip>
#include "team_manager.hpp"

// ---------------------------------------------------------------------------
//  Shared state — one double per pipeline stage per task
// ---------------------------------------------------------------------------
static std::atomic<double> src[7]{};   // src[task_id]: source output
static std::atomic<double> mid[7]{};   // mid[task_id]: intermediate output

// ---------------------------------------------------------------------------
//  Helper: build SubtaskInfo
// ---------------------------------------------------------------------------
static SubtaskInfo make_info(int task_id, int id, const char* type,
                              int core, int priority,
                              uint64_t period_ns, uint64_t deadline_ns) {
    SubtaskInfo info;
    info.task_id        = task_id;
    info.id             = id;
    info.component_type = type;
    info.host           = "localhost";
    info.core           = core;
    info.priority       = priority;
    info.period_ns      = period_ns;
    info.deadline_ns    = deadline_ns;
    return info;
}

// ---------------------------------------------------------------------------
//  Subtask factory for one task (task_id 1–6, subtask IDs base, base+1, base+2)
//  Returns {source, intermediate, sink}.
// ---------------------------------------------------------------------------
struct TaskSubtasks {
    Subtask source;
    Subtask intermediate;
    Subtask sink;
};

static TaskSubtasks make_task(int tid, int base_id) {
    return {
        Subtask(base_id,     [tid] {
            double v = src[tid].load() + 1.0;
            src[tid].store(v);
        }),
        Subtask(base_id + 1, [tid] {
            double v = src[tid].load() * 2.0;
            mid[tid].store(v);
        }),
        Subtask(base_id + 2, [tid] {
            std::cout << "  [task " << tid << " sink] "
                      << std::fixed << std::setprecision(0)
                      << mid[tid].load() << "\n";
        })
    };
}

int main() {
    // -----------------------------------------------------------------------
    //  Periods (nanoseconds) and priorities (RMS)
    // -----------------------------------------------------------------------
    constexpr uint64_t T[7] = { 0,
        1'000'000ULL,   // task 1 — 1 ms
        2'000'000ULL,   // task 2 — 2 ms
        6'000'000ULL,   // task 3 — 6 ms
       12'000'000ULL,   // task 4 — 12 ms
       24'000'000ULL,   // task 5 — 24 ms
       48'000'000ULL,   // task 6 — 48 ms
    };
    constexpr int PRIO[7] = { 0, 16, 14, 12, 10, 8, 6 };

    // -----------------------------------------------------------------------
    //  Create 18 subtasks (6 tasks × 3 stages)
    // -----------------------------------------------------------------------
    TaskSubtasks t1 = make_task(1,  1);
    TaskSubtasks t2 = make_task(2,  4);
    TaskSubtasks t3 = make_task(3,  7);
    TaskSubtasks t4 = make_task(4, 10);
    TaskSubtasks t5 = make_task(5, 13);
    TaskSubtasks t6 = make_task(6, 16);

    // -----------------------------------------------------------------------
    //  DAG: 18 nodes, 12 edges (6 disconnected linear chains)
    // -----------------------------------------------------------------------
    DAG dag;
    for (int i = 1; i <= 18; ++i) dag.add_node(i, nullptr);

    // Task 1: 1→2→3
    dag.add_edge(1, 2);   dag.add_edge(2,  3);
    // Task 2: 4→5→6
    dag.add_edge(4, 5);   dag.add_edge(5,  6);
    // Task 3: 7→8→9
    dag.add_edge(7, 8);   dag.add_edge(8,  9);
    // Task 4: 10→11→12
    dag.add_edge(10, 11); dag.add_edge(11, 12);
    // Task 5: 13→14→15
    dag.add_edge(13, 14); dag.add_edge(14, 15);
    // Task 6: 16→17→18
    dag.add_edge(16, 17); dag.add_edge(17, 18);

    // -----------------------------------------------------------------------
    //  TeamManager — one instance manages all 6 independent task chains
    // -----------------------------------------------------------------------
    TeamManager tm;
    tm.initialize({
        // task 1 (T=1ms, prio=16)
        {make_info(1,  1,  "source",       0, PRIO[1], T[1], T[1]), &t1.source},
        {make_info(1,  2,  "intermediate", 1, PRIO[1], T[1], T[1]), &t1.intermediate},
        {make_info(1,  3,  "sink",         2, PRIO[1], T[1], T[1]), &t1.sink},
        // task 2 (T=2ms, prio=14)
        {make_info(2,  4,  "source",       0, PRIO[2], T[2], T[2]), &t2.source},
        {make_info(2,  5,  "intermediate", 1, PRIO[2], T[2], T[2]), &t2.intermediate},
        {make_info(2,  6,  "sink",         2, PRIO[2], T[2], T[2]), &t2.sink},
        // task 3 (T=6ms, prio=12)
        {make_info(3,  7,  "source",       0, PRIO[3], T[3], T[3]), &t3.source},
        {make_info(3,  8,  "intermediate", 1, PRIO[3], T[3], T[3]), &t3.intermediate},
        {make_info(3,  9,  "sink",         2, PRIO[3], T[3], T[3]), &t3.sink},
        // task 4 (T=12ms, prio=10)
        {make_info(4, 10,  "source",       0, PRIO[4], T[4], T[4]), &t4.source},
        {make_info(4, 11,  "intermediate", 1, PRIO[4], T[4], T[4]), &t4.intermediate},
        {make_info(4, 12,  "sink",         2, PRIO[4], T[4], T[4]), &t4.sink},
        // task 5 (T=24ms, prio=8)
        {make_info(5, 13,  "source",       0, PRIO[5], T[5], T[5]), &t5.source},
        {make_info(5, 14,  "intermediate", 1, PRIO[5], T[5], T[5]), &t5.intermediate},
        {make_info(5, 15,  "sink",         2, PRIO[5], T[5], T[5]), &t5.sink},
        // task 6 (T=48ms, prio=6)
        {make_info(6, 16,  "source",       0, PRIO[6], T[6], T[6]), &t6.source},
        {make_info(6, 17,  "intermediate", 1, PRIO[6], T[6], T[6]), &t6.intermediate},
        {make_info(6, 18,  "sink",         2, PRIO[6], T[6], T[6]), &t6.sink},
    }, dag);

    // pipeline_depth = 3 (longest chain in each task = 3 nodes)
    // N = next_pow2(max(2, ceil(T/T) + 3)) = next_pow2(4) = 4 for all edges
    std::cout << "=== 6-task pipeline (RMS, 3 cores) ===\n"
              << "Tasks: 1ms/2ms/6ms/12ms/24ms/48ms  prio: 16/14/12/10/8/6\n"
              << "DAG: 18 nodes, 12 edges, 6 disconnected chains  depth=3\n"
              << "Dispatchers: " << tm.dispatcher_count()
              << "  (one per unique (core,priority) pair)\n\n"
              << "Ring buffer sizes (N = next_pow2(ceil(D/T) + depth)):\n";
    for (int i = 1; i <= 6; i++) {
        int base = (i - 1) * 3 + 1;
        std::cout << "  task " << i
                  << "  edge " << base   << "→" << base+1 << ": " << tm.ring_buffer_size(base,   base+1)
                  << "  edge " << base+1 << "→" << base+2 << ": " << tm.ring_buffer_size(base+1, base+2)
                  << "\n";
    }
    std::cout << "\n";

    tm.start();

    // -----------------------------------------------------------------------
    //  Drive tasks from the main thread.
    //  Tick = 1ms.  Run 4 × LCM(1,2,6,12,24,48) = 4 × 48 = 192 ticks.
    // -----------------------------------------------------------------------
    std::cout << "Running 192ms (4 × 48ms LCM)...\n";
    const int TICKS = 192;
    for (int tick = 1; tick <= TICKS; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        if (tick % 1  == 0) tm.notify(1);   // T=1ms
        if (tick % 2  == 0) tm.notify(4);   // T=2ms
        if (tick % 6  == 0) tm.notify(7);   // T=6ms
        if (tick % 12 == 0) tm.notify(10);  // T=12ms
        if (tick % 24 == 0) tm.notify(13);  // T=24ms
        if (tick % 48 == 0) tm.notify(16);  // T=48ms
    }

    // Allow in-flight subtasks to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.stop();

    std::cout << "\n=== Summary: activations per task ===\n";
    std::cout << "  task 1 (1ms):  " << (int)src[1].load() << " activations\n";
    std::cout << "  task 2 (2ms):  " << (int)src[2].load() << " activations\n";
    std::cout << "  task 3 (6ms):  " << (int)src[3].load() << " activations\n";
    std::cout << "  task 4 (12ms): " << (int)src[4].load() << " activations\n";
    std::cout << "  task 5 (24ms): " << (int)src[5].load() << " activations\n";
    std::cout << "  task 6 (48ms): " << (int)src[6].load() << " activations\n";
    std::cout << "Done.\n";
    return 0;
}
