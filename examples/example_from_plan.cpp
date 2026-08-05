/**
 * example_from_plan.cpp
 *
 * Reads a deployment plan JSON and executes the pipeline dynamically,
 * without code generation (Phase 6 pending).
 *
 * Usage: ./example_from_plan <deployment_plan.json> [hyperperiods]
 *
 * Generic component semantics (demo):
 *   source       — increments a per-subtask counter
 *   intermediate — doubles its predecessor's value
 *   sink         — prints its predecessor's value
 *
 * The main thread fires source subtasks at their declared period_ns for
 * hyperperiods × LCM(all source periods), defaulting to 4.
 */

#include <iostream>
#include <iomanip>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <map>
#include <memory>
#include <vector>
#include "team_manager.hpp"
#include "parser_json.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <deployment_plan.json> [hyperperiods]\n"
                  << "  hyperperiods: number of hyperperiods to simulate (default 4)\n";
        return 1;
    }

    // Number of hyperperiods to simulate; more => more ticks (longer run).
    uint64_t hyperperiods = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 4;
    if (hyperperiods == 0) hyperperiods = 4;

    // -----------------------------------------------------------------------
    //  1. Parse deployment plan
    // -----------------------------------------------------------------------
    JsonParser parser;
    DeploymentPlan plan = parser.parse(argv[1]);

    // -----------------------------------------------------------------------
    //  2. Predecessor map: downstream_id → [upstream_ids]
    // -----------------------------------------------------------------------
    std::map<int, std::vector<int>> preds;
    for (auto& conn : plan.connections)
        preds[conn.downstream].push_back(conn.upstream);

    // -----------------------------------------------------------------------
    //  3. Shared state: one atomic<double> per subtask slot
    // -----------------------------------------------------------------------
    int max_id = 0;
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            max_id = std::max(max_id, st.id);

    auto vals_buf = std::make_unique<std::atomic<double>[]>(max_id + 1);
    for (int i = 0; i <= max_id; ++i)
        vals_buf[i].store(0.0, std::memory_order_relaxed);
    auto* v = vals_buf.get();

    // -----------------------------------------------------------------------
    //  4. Create Subtask objects (non-copyable → heap via unique_ptr)
    // -----------------------------------------------------------------------
    std::map<int, std::unique_ptr<Subtask>> subtask_ptrs;

    for (auto& task : plan.tasks) {
        for (auto& info : task.subtasks) {
            int id = info.id;
            if (info.component_type == "source") {
                subtask_ptrs[id] = std::make_unique<Subtask>(id,
                    [v, id] {
                        v[id].store(v[id].load(std::memory_order_relaxed) + 1.0,
                                    std::memory_order_relaxed);
                    });
            } else if (info.component_type == "intermediate") {
                int pred = preds.at(id)[0];
                subtask_ptrs[id] = std::make_unique<Subtask>(id,
                    [v, id, pred] {
                        v[id].store(v[pred].load(std::memory_order_relaxed) * 2.0,
                                    std::memory_order_relaxed);
                    });
            } else { // sink
                int pred = preds.at(id)[0];
                subtask_ptrs[id] = std::make_unique<Subtask>(id,
                    [v, id, pred] {
                        std::cout << "  [sink " << id << "] "
                                  << std::fixed << std::setprecision(0)
                                  << v[pred].load(std::memory_order_relaxed) << "\n";
                    });
            }
        }
    }

    // -----------------------------------------------------------------------
    //  5. Build DAG from plan
    // -----------------------------------------------------------------------
    DAG dag;
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            dag.add_node(st.id, nullptr);
    for (auto& conn : plan.connections)
        dag.add_edge(conn.upstream, conn.downstream);

    // -----------------------------------------------------------------------
    //  6. TeamManager: initialize
    // -----------------------------------------------------------------------
    std::vector<TeamManager::SubtaskEntry> entries;
    for (auto& task : plan.tasks)
        for (auto& info : task.subtasks)
            entries.push_back({info, subtask_ptrs.at(info.id).get()});

    TeamManager tm;
    tm.initialize(entries, dag);

    std::cout << "=== " << argv[1] << " ===\n"
              << "Tasks: "       << plan.tasks.size()
              << "  Subtasks: "  << entries.size()
              << "  Edges: "     << plan.connections.size() << "\n"
              << "Dispatchers: " << tm.dispatcher_count()
              << "  (one per unique (core,priority) pair)\n\n"
              << "Ring buffer sizes (N = next_pow2(ceil(D/T)+depth)):\n";
    for (auto& conn : plan.connections) {
        std::cout << "  " << conn.upstream << " → " << conn.downstream
                  << ": N=" << tm.ring_buffer_size(conn.upstream, conn.downstream)
                  << "\n";
    }
    std::cout << "\n";

    // -----------------------------------------------------------------------
    //  7. Compute tick parameters from source subtasks
    // -----------------------------------------------------------------------
    std::vector<std::pair<int, uint64_t>> sources; // {id, period_ns}
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (preds.find(st.id) == preds.end())
                sources.push_back({st.id, st.period_ns});

    uint64_t min_p = sources[0].second;
    for (auto& [id, p] : sources)
        min_p = std::min(min_p, p);

    uint64_t lcm_p = sources[0].second;
    for (std::size_t i = 1; i < sources.size(); ++i)
        lcm_p = std::lcm(lcm_p, sources[i].second);

    const uint64_t ticks = hyperperiods * lcm_p / min_p;

    std::cout << "Tick = "    << min_p / 1'000'000ULL << " ms"
              << "  LCM = "   << lcm_p / 1'000'000ULL << " ms"
              << "  Hyperperiods = " << hyperperiods
              << "  Running " << ticks << " ticks ("
              << ticks * min_p / 1'000'000ULL << " ms)...\n";

    // -----------------------------------------------------------------------
    //  8. Start, tick loop, stop
    // -----------------------------------------------------------------------
    tm.start();

    for (uint64_t tick = 1; tick <= ticks; ++tick) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(min_p));
        for (auto& [id, p] : sources)
            if (tick % (p / min_p) == 0)
                tm.notify(id);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.stop();

    // -----------------------------------------------------------------------
    //  9. Summary
    // -----------------------------------------------------------------------
    std::cout << "\n=== Summary: source activations ===\n";
    for (auto& [id, p] : sources)
        std::cout << "  subtask " << id
                  << " (T=" << p / 1'000'000ULL << " ms): "
                  << static_cast<int>(v[id].load()) << " activations\n";
    std::cout << "Done.\n";
    return 0;
}
