/**
 * example_eval.cpp
 *
 * Evaluates scheduling quality for each subtask in a deployment plan:
 *
 *   Latency  = t_actual - t_scheduled
 *            = time the scheduler actually started the subtask
 *              minus the time it was supposed to start.
 *            Always >= 0 in a causal system. Lower is better.
 *
 *   Jitter   = variation of the latency across jobs of the same subtask.
 *            Reported as variance (σ²) and standard deviation (σ).
 *
 * How the values are derived:
 *   The dispatcher runs step 4b BEFORE calling execute():
 *     s->next_release_ns += s->period_ns
 *   Therefore, inside execute():
 *     t_scheduled = s->next_release_ns - s->period_ns
 *     t_actual    = Dispatcher::monotonic_ns()  (captured at execute() entry)
 *     latency     = t_actual - t_scheduled
 *
 * Usage:
 *   ./example_eval <deployment_plan.json>
 *   sudo ./example_eval ...    (enables SCHED_FIFO; cleaner measurements)
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
#include "team_manager.hpp"
#include "parser_json.hpp"

// ---------------------------------------------------------------------------
//  Per-subtask metrics
// ---------------------------------------------------------------------------
struct SubtaskMetrics {
    int      id        = 0;
    uint64_t period_ns = 0;
    int      core      = 0;
    int      priority  = 0;

    // One entry per job: t_actual - t_scheduled (nanoseconds)
    std::vector<int64_t> latency_ns;

    int deadline_misses = 0; // jobs where latency > period
};

// ---------------------------------------------------------------------------
//  Statistics helpers
// ---------------------------------------------------------------------------
static double ns_to_us(double ns) { return ns / 1000.0; }

static double mean(const std::vector<int64_t>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (auto x : v) s += static_cast<double>(x);
    return s / static_cast<double>(v.size());
}

// Peak-to-peak jitter: max - min
static int64_t jitter(const std::vector<int64_t>& v) {
    if (v.size() < 2) return 0;
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
}

static int64_t vmin(const std::vector<int64_t>& v) {
    return v.empty() ? 0 : *std::min_element(v.begin(), v.end());
}

static int64_t vmax(const std::vector<int64_t>& v) {
    return v.empty() ? 0 : *std::max_element(v.begin(), v.end());
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
    //  3. Shared state: one atomic<double> slot per subtask id
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
    //  4. Tick loop parameters (needed before reserve())
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
    //  5. Phase 1 — allocate Subtask objects and pre-reserve metrics vectors
    // -----------------------------------------------------------------------
    std::map<int, SubtaskMetrics>           mmap;
    std::map<int, std::unique_ptr<Subtask>> subtask_ptrs;

    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            subtask_ptrs[st.id] = std::make_unique<Subtask>(st.id, []{});

            auto& m   = mmap[st.id];
            m.id        = st.id;
            m.period_ns = st.period_ns;
            m.core      = st.core;
            m.priority  = st.priority;

            // Max capacity = expected number of jobs for this period
            int cap = (st.period_ns > 0)
                ? static_cast<int>(4 * lcm_p / st.period_ns) + 4
                : ticks + 4;
            m.latency_ns.reserve(cap);
        }
    }

    // -----------------------------------------------------------------------
    //  6. Phase 2 — assign instrumented execute() lambdas
    //
    //  Two-phase pattern: the Subtask already lives on the heap (stable address),
    //  so we can safely capture 's' as a raw pointer inside the lambda.
    //  This lets us read s->next_release_ns and s->period_ns at execution time.
    // -----------------------------------------------------------------------
    for (auto& task : plan.tasks) {
        for (auto& info : task.subtasks) {
            int      id = info.id;
            Subtask*  s = subtask_ptrs.at(id).get();
            auto&     m = mmap.at(id);

            if (info.component_type == "source") {

                s->execute = [v, id, s, &m] {
                    uint64_t t_actual = Dispatcher::monotonic_ns();

                    v[id].store(v[id].load(std::memory_order_relaxed) + 1.0,
                                std::memory_order_relaxed);

                    if (s->period_ns > 0) {
                        int64_t t_sched = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                        m.latency_ns.push_back(lat);
                        if (static_cast<uint64_t>(lat) > s->period_ns)
                            ++m.deadline_misses;
                    }
                };

            } else if (info.component_type == "intermediate") {
                int pred = preds.at(id)[0];

                s->execute = [v, id, pred, s, &m] {
                    uint64_t t_actual = Dispatcher::monotonic_ns();

                    v[id].store(v[pred].load(std::memory_order_relaxed) * 2.0,
                                std::memory_order_relaxed);

                    if (s->period_ns > 0) {
                        int64_t t_sched = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                        m.latency_ns.push_back(lat);
                        if (static_cast<uint64_t>(lat) > s->period_ns)
                            ++m.deadline_misses;
                    }
                };

            } else { // sink
                int pred = preds.at(id)[0];

                s->execute = [v, pred, s, &m] {
                    uint64_t t_actual = Dispatcher::monotonic_ns();

                    (void)v[pred].load(std::memory_order_relaxed);

                    if (s->period_ns > 0) {
                        int64_t t_sched = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                        m.latency_ns.push_back(lat);
                        if (static_cast<uint64_t>(lat) > s->period_ns)
                            ++m.deadline_misses;
                    }
                };
            }
        }
    }

    // -----------------------------------------------------------------------
    //  7. Build DAG
    // -----------------------------------------------------------------------
    DAG dag;
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            dag.add_node(st.id, nullptr);
    for (auto& conn : plan.connections)
        dag.add_edge(conn.upstream, conn.downstream);

    // -----------------------------------------------------------------------
    //  8. TeamManager
    // -----------------------------------------------------------------------
    std::vector<TeamManager::SubtaskEntry> entries;
    for (auto& task : plan.tasks)
        for (auto& info : task.subtasks)
            entries.push_back({info, subtask_ptrs.at(info.id).get()});

    TeamManager tm;
    tm.initialize(entries, dag);

    std::cout << "=== Scheduling Evaluation: " << argv[1] << " ===\n"
              << "Tasks: "         << plan.tasks.size()
              << "  Subtasks: "    << entries.size()
              << "  Dispatchers: " << tm.dispatcher_count() << "\n"
              << "Tick: "          << min_p / 1'000'000ULL << " ms"
              << "  LCM: "         << lcm_p / 1'000'000ULL << " ms"
              << "  Ticks: "       << ticks
              << "  (" << ticks * min_p / 1'000'000ULL << " ms)\n"
              << "Collecting metrics (no output during run)...\n\n";

    // -----------------------------------------------------------------------
    //  9. Run
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
    //  10. Report
    // -----------------------------------------------------------------------
    const int W = 16;
    std::cout << std::fixed << std::setprecision(3);

    std::cout << "=== Latency & Jitter per Subtask ===\n";
    std::cout << std::left
              << std::setw(4)  << "ID"
              << std::setw(12) << "Period(ms)"
              << std::setw(6)  << "Core"
              << std::setw(6)  << "Prio"
              << std::setw(6)  << "Jobs"
              << std::setw(W)  << "Lat_min(us)"
              << std::setw(W)  << "Lat_mean(us)"
              << std::setw(W)  << "Lat_max(us)"
              << std::setw(W)  << "Jitter(us)"
              << "Misses\n"
              << std::string(4 + 12 + 6 + 6 + 6 + W * 4 + 6, '-') << "\n";

    int total_misses = 0;
    int total_jobs   = 0;

    for (auto& [id, m] : mmap) {
        auto& lv = m.latency_ns;
        total_misses += m.deadline_misses;
        total_jobs   += static_cast<int>(lv.size());

        std::cout << std::left
                  << std::setw(4)  << m.id
                  << std::setw(12) << (m.period_ns / 1'000'000ULL)
                  << std::setw(6)  << m.core
                  << std::setw(6)  << m.priority
                  << std::setw(6)  << static_cast<int>(lv.size())
                  << std::setw(W)  << ns_to_us(static_cast<double>(vmin(lv)))
                  << std::setw(W)  << ns_to_us(mean(lv))
                  << std::setw(W)  << ns_to_us(static_cast<double>(vmax(lv)))
                  << std::setw(W)  << ns_to_us(static_cast<double>(jitter(lv)))
                  << m.deadline_misses << "\n";
    }

    std::cout << "\nTotal jobs: "      << total_jobs
              << "  Deadline misses: " << total_misses
              << "  Miss rate: "
              << std::setprecision(2)
              << (total_jobs > 0 ? 100.0 * total_misses / total_jobs : 0.0)
              << "%\n";

    return 0;
}
