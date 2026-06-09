/**
 * example_eval.cpp
 *
 * Evaluates scheduling quality of a deployment plan JSON:
 *   - Release jitter (actual start vs expected release time, per subtask)
 *   - Execution time per subtask
 *   - Deadline misses (finish time > deadline of the current job)
 *   - End-to-end pipeline latency per task (source start → sink finish)
 *
 * Usage:  ./example_eval <deployment_plan.json>
 *         sudo ./example_eval ...   (SCHED_FIFO requires root / CAP_SYS_NICE)
 *
 * Timing reference: CLOCK_MONOTONIC nanoseconds (Dispatcher::monotonic_ns()).
 *
 * Jitter derivation
 * -----------------
 * Dispatcher::process_subtask() runs step 4b BEFORE calling execute():
 *   s->next_release_ns += s->period_ns          (or = now + period on first job)
 * So at the moment execute() starts:
 *   expected_release = s->next_release_ns - s->period_ns
 *   deadline         = s->next_release_ns
 *   jitter           = t_start - expected_release
 *   miss             = t_end   > deadline
 */

#include <iostream>
#include <iomanip>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <map>
#include <memory>
#include <vector>
#include <cmath>
#include "team_manager.hpp"
#include "parser_json.hpp"

// ---------------------------------------------------------------------------
//  Per-subtask metrics (pre-allocated; no dynamic allocation in RT loop)
// ---------------------------------------------------------------------------
struct SubtaskMetrics {
    int      id        = 0;
    uint64_t period_ns = 0;
    int      core      = 0;
    int      priority  = 0;

    std::vector<int64_t>  jitter_ns;   // signed: t_start − expected_release
    std::vector<uint64_t> exec_ns;     // t_end − t_start
    std::vector<int64_t>  latency_ns;  // sink only: t_sink_end − t_source_start
    int deadline_misses = 0;
};

// ---------------------------------------------------------------------------
//  Statistics helpers
// ---------------------------------------------------------------------------
static double ns_to_us(double ns) { return ns / 1000.0; }

static double vmean(const std::vector<int64_t>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0; for (auto x : v) s += static_cast<double>(x); return s / v.size();
}
static double vmean(const std::vector<uint64_t>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0; for (auto x : v) s += static_cast<double>(x); return s / v.size();
}
static int64_t vmax(const std::vector<int64_t>& v) {
    if (v.empty()) return 0;
    return *std::max_element(v.begin(), v.end());
}
static int64_t vmin(const std::vector<int64_t>& v) {
    if (v.empty()) return 0;
    return *std::min_element(v.begin(), v.end());
}
static double vstddev(const std::vector<int64_t>& v) {
    if (v.size() < 2) return 0.0;
    double m = vmean(v), s = 0.0;
    for (auto x : v) { double d = static_cast<double>(x) - m; s += d * d; }
    return std::sqrt(s / static_cast<double>(v.size()));
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <deployment_plan.json>\n"
                  << "       sudo " << argv[0] << " ...  (for SCHED_FIFO)\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    //  1. Parse plan
    // -----------------------------------------------------------------------
    JsonParser parser;
    DeploymentPlan plan = parser.parse(argv[1]);

    // -----------------------------------------------------------------------
    //  2. Predecessor / successor maps
    // -----------------------------------------------------------------------
    std::map<int, std::vector<int>> preds, succs;
    for (auto& conn : plan.connections) {
        preds[conn.downstream].push_back(conn.upstream);
        succs[conn.upstream].push_back(conn.downstream);
    }

    // -----------------------------------------------------------------------
    //  3. Source subtask per task  +  subtask → task mapping
    // -----------------------------------------------------------------------
    std::map<int, int> task_source_id;  // task_id   → source subtask_id
    std::map<int, int> subtask_task_id; // subtask_id → task_id

    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            subtask_task_id[st.id] = task.id;
            if (preds.find(st.id) == preds.end())
                task_source_id[task.id] = st.id;
        }
    }

    // -----------------------------------------------------------------------
    //  4. Shared buffers
    // -----------------------------------------------------------------------
    int max_id = 0;
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            max_id = std::max(max_id, st.id);

    auto vals_buf = std::make_unique<std::atomic<double>[]>(max_id + 1);
    auto fire_buf = std::make_unique<std::atomic<uint64_t>[]>(max_id + 1);
    for (int i = 0; i <= max_id; ++i) {
        vals_buf[i].store(0.0, std::memory_order_relaxed);
        fire_buf[i].store(0ULL, std::memory_order_relaxed);
    }
    auto* v    = vals_buf.get();
    auto* fire = fire_buf.get();

    // -----------------------------------------------------------------------
    //  5. Tick parameters (needed for reserve())
    // -----------------------------------------------------------------------
    std::vector<std::pair<int, uint64_t>> sources; // {id, period_ns}
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (preds.find(st.id) == preds.end())
                sources.push_back({st.id, st.period_ns});

    uint64_t min_p = sources[0].second;
    for (auto& [id, p] : sources) min_p = std::min(min_p, p);

    uint64_t lcm_p = sources[0].second;
    for (std::size_t i = 1; i < sources.size(); ++i)
        lcm_p = std::lcm(lcm_p, sources[i].second);

    int ticks = static_cast<int>(4 * lcm_p / min_p);

    // -----------------------------------------------------------------------
    //  6. Phase 1 — allocate Subtasks + pre-allocate metrics vectors
    // -----------------------------------------------------------------------
    std::map<int, SubtaskMetrics> mmap;
    std::map<int, std::unique_ptr<Subtask>> subtask_ptrs;

    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            subtask_ptrs[st.id] = std::make_unique<Subtask>(st.id, []{});
            auto& m  = mmap[st.id];
            m.id       = st.id;
            m.period_ns = st.period_ns;
            m.core     = st.core;
            m.priority = st.priority;
            int cap = (st.period_ns > 0)
                ? static_cast<int>(4 * lcm_p / st.period_ns) + 8
                : ticks + 8;
            m.jitter_ns.reserve(cap);
            m.exec_ns.reserve(cap);
            m.latency_ns.reserve(cap);
        }
    }

    // -----------------------------------------------------------------------
    //  7. Phase 2 — wire instrumented execute() lambdas
    //
    //  Captures:
    //    v, fire  — raw pointers (stable for lifetime of main)
    //    s        — Subtask* raw ptr (stable: unique_ptr, never moved)
    //    &m       — reference to SubtaskMetrics (stable: std::map, never rehashes)
    //    pred, src_id — int values by copy
    // -----------------------------------------------------------------------
    for (auto& task : plan.tasks) {
        for (auto& info : task.subtasks) {
            int id = info.id;
            Subtask* s = subtask_ptrs.at(id).get();
            auto& m    = mmap.at(id);

            if (info.component_type == "source") {
                s->execute = [v, fire, id, s, &m] {
                    uint64_t t_start = Dispatcher::monotonic_ns();
                    fire[id].store(t_start, std::memory_order_release);

                    v[id].store(v[id].load(std::memory_order_relaxed) + 1.0,
                                std::memory_order_relaxed);

                    uint64_t t_end = Dispatcher::monotonic_ns();
                    m.exec_ns.push_back(t_end - t_start);
                    if (s->period_ns > 0) {
                        int64_t expected = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        m.jitter_ns.push_back(
                            static_cast<int64_t>(t_start) - expected);
                        if (t_end > s->next_release_ns) ++m.deadline_misses;
                    }
                };

            } else if (info.component_type == "intermediate") {
                int pred = preds.at(id)[0];
                s->execute = [v, id, pred, s, &m] {
                    uint64_t t_start = Dispatcher::monotonic_ns();

                    v[id].store(v[pred].load(std::memory_order_relaxed) * 2.0,
                                std::memory_order_relaxed);

                    uint64_t t_end = Dispatcher::monotonic_ns();
                    m.exec_ns.push_back(t_end - t_start);
                    if (s->period_ns > 0) {
                        int64_t expected = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        m.jitter_ns.push_back(
                            static_cast<int64_t>(t_start) - expected);
                        if (t_end > s->next_release_ns) ++m.deadline_misses;
                    }
                };

            } else { // sink — no cout, just metrics
                int pred   = preds.at(id)[0];
                int src_id = task_source_id.at(subtask_task_id.at(id));
                s->execute = [v, fire, id, pred, src_id, s, &m] {
                    uint64_t t_start = Dispatcher::monotonic_ns();

                    (void)v[pred].load(std::memory_order_relaxed); // consume

                    uint64_t t_end = Dispatcher::monotonic_ns();
                    m.exec_ns.push_back(t_end - t_start);
                    if (s->period_ns > 0) {
                        int64_t expected = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        m.jitter_ns.push_back(
                            static_cast<int64_t>(t_start) - expected);
                        if (t_end > s->next_release_ns) ++m.deadline_misses;
                    }
                    uint64_t src_fire = fire[src_id].load(
                        std::memory_order_acquire);
                    if (src_fire > 0)
                        m.latency_ns.push_back(
                            static_cast<int64_t>(t_end - src_fire));
                };
            }
        }
    }

    // -----------------------------------------------------------------------
    //  8. Build DAG
    // -----------------------------------------------------------------------
    DAG dag;
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            dag.add_node(st.id, nullptr);
    for (auto& conn : plan.connections)
        dag.add_edge(conn.upstream, conn.downstream);

    // -----------------------------------------------------------------------
    //  9. TeamManager
    // -----------------------------------------------------------------------
    std::vector<TeamManager::SubtaskEntry> entries;
    for (auto& task : plan.tasks)
        for (auto& info : task.subtasks)
            entries.push_back({info, subtask_ptrs.at(info.id).get()});

    TeamManager tm;
    tm.initialize(entries, dag);

    std::cout << "=== Scheduling Evaluation: " << argv[1] << " ===\n"
              << "Tasks: "        << plan.tasks.size()
              << "  Subtasks: "   << entries.size()
              << "  Dispatchers: "<< tm.dispatcher_count() << "\n"
              << "Tick: "         << min_p / 1'000'000ULL << " ms"
              << "  LCM: "        << lcm_p / 1'000'000ULL << " ms"
              << "  Ticks: "      << ticks
              << "  (" << ticks * min_p / 1'000'000ULL << " ms)\n"
              << "Collecting metrics (no output during run)...\n\n";

    // -----------------------------------------------------------------------
    //  10. Run
    // -----------------------------------------------------------------------
    tm.start();

    for (int tick = 1; tick <= ticks; ++tick) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(min_p));
        for (auto& [id, p] : sources)
            if (static_cast<uint64_t>(tick) % (p / min_p) == 0)
                tm.notify(id);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.stop();

    // -----------------------------------------------------------------------
    //  11. Report
    // -----------------------------------------------------------------------
    const int W = 18;
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "=== Per-subtask Metrics ===\n";
    std::cout << std::left
              << std::setw(4)  << "ID"
              << std::setw(12) << "Period(ms)"
              << std::setw(6)  << "Core"
              << std::setw(6)  << "Prio"
              << std::setw(6)  << "Jobs"
              << std::setw(W)  << "Jitter_mean(us)"
              << std::setw(W)  << "Jitter_max(us)"
              << std::setw(W)  << "Jitter_std(us)"
              << std::setw(W)  << "Exec_mean(us)"
              << "Misses\n"
              << std::string(6 + 12 + 6 + 6 + 6 + W*4 + 6, '-') << "\n";

    for (auto& [id, m] : mmap) {
        std::cout << std::left
                  << std::setw(4)  << m.id
                  << std::setw(12) << (m.period_ns / 1'000'000ULL)
                  << std::setw(6)  << m.core
                  << std::setw(6)  << m.priority
                  << std::setw(6)  << static_cast<int>(m.exec_ns.size())
                  << std::setw(W)  << ns_to_us(vmean(m.jitter_ns))
                  << std::setw(W)  << ns_to_us(vmax(m.jitter_ns))
                  << std::setw(W)  << ns_to_us(vstddev(m.jitter_ns))
                  << std::setw(W)  << ns_to_us(vmean(m.exec_ns))
                  << m.deadline_misses << "\n";
    }

    std::cout << "\n=== End-to-end Latency (source start → sink finish) ===\n";
    std::cout << std::left
              << std::setw(6)  << "Task"
              << std::setw(12) << "Period(ms)"
              << std::setw(6)  << "Jobs"
              << std::setw(W)  << "Latency_min(us)"
              << std::setw(W)  << "Latency_mean(us)"
              << "Latency_max(us)\n"
              << std::string(6 + 12 + 6 + W*3, '-') << "\n";

    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            if (succs.find(st.id) == succs.end()) { // no successors → sink
                auto& lv = mmap.at(st.id).latency_ns;
                if (!lv.empty()) {
                    std::cout << std::left
                              << std::setw(6)  << task.id
                              << std::setw(12) << (st.period_ns / 1'000'000ULL)
                              << std::setw(6)  << static_cast<int>(lv.size())
                              << std::setw(W)  << ns_to_us(vmin(lv))
                              << std::setw(W)  << ns_to_us(vmean(lv))
                              << ns_to_us(vmax(lv)) << "\n";
                }
                break;
            }
        }
    }

    int total_misses = 0, total_jobs = 0;
    for (auto& [id, m] : mmap) {
        total_misses += m.deadline_misses;
        total_jobs   += static_cast<int>(m.exec_ns.size());
    }
    std::cout << "\nTotal jobs: " << total_jobs
              << "  Deadline misses: " << total_misses
              << "  Miss rate: "
              << (total_jobs > 0 ? 100.0 * total_misses / total_jobs : 0.0)
              << "%\n";

    return 0;
}
