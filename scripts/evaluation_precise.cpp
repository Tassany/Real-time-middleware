/**
 * evaluation_precise.cpp
 *
 * Same instrumentation as evaluation.cpp (per-subtask Latency/Jitter and
 * per-task end-to-end Response Time — see that file's header for the exact
 * definitions, unchanged here), but with a different release mechanism.
 *
 * evaluation.cpp drives all sources off one shared tick counter: every
 * min(all periods) it checks `tick % (period / min_p) == 0`. That division
 * is exact only when every source period is a whole multiple of min_p ns.
 * Rational Hz values rarely are (e.g. 1/70Hz = 14.2857...ms against a 5ms
 * tick truncates to a ratio of 2, so the source actually fires every 10ms —
 * a silent 30% shorter period, i.e. genuinely more load than the plan
 * declares, not just a measurement quirk).
 *
 * This file instead gives each source its own absolute next-fire time
 * (next_fire_ns += period_ns, exactly, every time — the same pattern
 * Dispatcher::process_subtask already uses internally for next_release_ns)
 * and always sleeps until whichever source is due next. No shared tick, no
 * LCM, no rounding, for any combination of periods.
 *
 * Kept as a separate binary rather than folded into evaluation.cpp so the
 * existing tick-based harness (already exercised by this project's other
 * plans) stays exactly as it was.
 *
 * Usage:
 *   ./evaluation_precise <plans/deployment_plan.json> <duration_ms>
 *   sudo ./evaluation_precise ...    (enables SCHED_FIFO; cleaner measurements)
 */

#include <iostream>
#include <iomanip>
#include <atomic>
#include <thread>
#include <chrono>
#include <time.h>
#include <algorithm>
#include <numeric>
#include <map>
#include <memory>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <mutex>
#include <deque>
#include <tuple>
#include "team_manager.hpp"
#include "parser_json.hpp"
#include "bench_registry.hpp"

// ---------------------------------------------------------------------------
//  Per-subtask metrics (identical to evaluation.cpp)
// ---------------------------------------------------------------------------
struct SubtaskMetrics {
    int      id          = 0;
    uint64_t period_ns   = 0;
    uint64_t deadline_ns = 0;
    int      core        = 0;
    int      priority    = 0;
    std::vector<int64_t> latency_ns;
    int deadline_misses = 0;
};

// ---------------------------------------------------------------------------
//  Per-task end-to-end response time (identical to evaluation.cpp)
// ---------------------------------------------------------------------------
struct TaskMetrics {
    uint64_t deadline_ns = 0;
    std::mutex mtx;
    std::deque<uint64_t> pending_release_ns;
    std::vector<int64_t> response_ns;
    int deadline_misses = 0;
};

// ---------------------------------------------------------------------------
//  Statistics helpers (identical to evaluation.cpp)
// ---------------------------------------------------------------------------
static double ns_to_us(double ns) { return ns / 1000.0; }

static double mean(const std::vector<int64_t>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (auto x : v) s += static_cast<double>(x);
    return s / static_cast<double>(v.size());
}

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
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <plans/deployment_plan.json> <duration_ms>\n"
                  << "       sudo " << argv[0] << " ...  (for SCHED_FIFO)\n"
                  << "  duration_ms: how long to run, wall-clock. Unlike evaluation.cpp\n"
                  << "  there is no hyperperiods/LCM concept here — see this file's header.\n";
        return 1;
    }

    uint64_t duration_ms = std::strtoull(argv[2], nullptr, 10);
    if (duration_ms == 0) { std::cerr << "duration_ms must be > 0\n"; return 1; }

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
    //  4. Sources: one absolute next-fire time each, no shared tick/LCM.
    // -----------------------------------------------------------------------
    struct SourceSched {
        int      id;
        uint64_t period_ns;
        int      task_id;
        uint64_t next_fire_ns; // set once tm.start() has run, see section 9
    };
    std::vector<SourceSched> sources;
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (preds.find(st.id) == preds.end())
                sources.push_back({st.id, st.period_ns, st.task_id, 0});
    if (sources.empty()) { std::cerr << "plan has no source subtasks\n"; return 1; }

    // -----------------------------------------------------------------------
    //  5. Phase 1 — allocate Subtask objects and pre-reserve metrics vectors
    // -----------------------------------------------------------------------
    std::map<int, SubtaskMetrics>           mmap;
    std::map<int, std::unique_ptr<Subtask>> subtask_ptrs;

    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            subtask_ptrs[st.id] = std::make_unique<Subtask>(st.id, []{});

            auto& m       = mmap[st.id];
            m.id          = st.id;
            m.period_ns   = st.period_ns;
            m.deadline_ns = st.deadline_ns;
            m.core        = st.core;
            m.priority    = st.priority;

            int cap = (st.period_ns > 0)
                ? static_cast<int>(duration_ms * 1'000'000ULL / st.period_ns) + 4
                : 4;
            m.latency_ns.reserve(cap);
        }
    }

    std::map<int, TaskMetrics> task_mmap;
    for (auto& task : plan.tasks) {
        auto& tm_entry = task_mmap[task.id];
        for (auto& st : task.subtasks)
            if (st.component_type == "sink") tm_entry.deadline_ns = st.deadline_ns;
    }

    // -----------------------------------------------------------------------
    //  6. Phase 2 — assign instrumented execute() lambdas
    //  (identical to evaluation.cpp; see that file for the per-branch comments)
    // -----------------------------------------------------------------------
    for (auto& task : plan.tasks) {
        for (auto& info : task.subtasks) {
            int      id = info.id;
            Subtask*  s = subtask_ptrs.at(id).get();
            auto&     m = mmap.at(id);

            bench::entry_fn body = info.benchmark.empty()
                                 ? nullptr
                                 : bench::lookup(info.benchmark);
            if (!info.benchmark.empty() && !body)
                throw std::runtime_error("subtask " + std::to_string(id) +
                    ": unknown benchmark '" + info.benchmark +
                    "' (expected " + bench::known_names() + ")");

            if (info.component_type == "source") {
                uint64_t dl = info.deadline_ns;

                s->execute = [v, id, s, &m, dl, body] {
                    uint64_t t_actual = Dispatcher::monotonic_ns();

                    v[id].store(v[id].load(std::memory_order_relaxed) + 1.0,
                                std::memory_order_relaxed);
                    if (body) body();

                    if (s->period_ns > 0) {
                        int64_t t_sched = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                        m.latency_ns.push_back(lat);
                        if (static_cast<uint64_t>(lat) > dl)
                            ++m.deadline_misses;
                    }
                };

            } else if (info.component_type == "intermediate") {
                std::vector<int> ups = preds.at(id);
                uint64_t         dl  = info.deadline_ns;

                s->execute = [v, id, ups, s, &m, dl, body] {
                    uint64_t t_actual = Dispatcher::monotonic_ns();

                    double sum = 0.0;
                    for (int p : ups)
                        sum += v[p].load(std::memory_order_relaxed);
                    v[id].store(sum * 2.0, std::memory_order_relaxed);
                    if (body) body();

                    if (s->period_ns > 0) {
                        int64_t t_sched = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                        m.latency_ns.push_back(lat);
                        if (static_cast<uint64_t>(lat) > dl)
                            ++m.deadline_misses;
                    }
                };

            } else { // sink
                std::vector<int> ups   = preds.at(id);
                uint64_t         dl    = info.deadline_ns;
                TaskMetrics&     tmet  = task_mmap.at(info.task_id);

                s->execute = [v, ups, s, &m, dl, body, &tmet] {
                    uint64_t t_actual = Dispatcher::monotonic_ns();

                    for (int p : ups)
                        (void)v[p].load(std::memory_order_relaxed);
                    if (body) body();

                    uint64_t t_done = Dispatcher::monotonic_ns();

                    if (s->period_ns > 0) {
                        int64_t t_sched = static_cast<int64_t>(
                            s->next_release_ns - s->period_ns);
                        int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                        m.latency_ns.push_back(lat);
                        if (static_cast<uint64_t>(lat) > dl)
                            ++m.deadline_misses;
                    }

                    std::lock_guard<std::mutex> lk(tmet.mtx);
                    if (!tmet.pending_release_ns.empty()) {
                        uint64_t rel = tmet.pending_release_ns.front();
                        tmet.pending_release_ns.pop_front();
                        int64_t resp = static_cast<int64_t>(t_done) - static_cast<int64_t>(rel);
                        tmet.response_ns.push_back(resp);
                        if (tmet.deadline_ns > 0 && static_cast<uint64_t>(resp) > tmet.deadline_ns)
                            ++tmet.deadline_misses;
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

    std::cout << "=== Scheduling Evaluation (precise): " << argv[1] << " ===\n"
              << "Tasks: "         << plan.tasks.size()
              << "  Subtasks: "    << entries.size()
              << "  Dispatchers: " << tm.dispatcher_count() << "\n"
              << "Duration: "      << duration_ms << " ms"
              << "  Sources: "     << sources.size() << "\n"
              << "Collecting metrics (no output during run)...\n\n";

    // -----------------------------------------------------------------------
    //  8b. Warm up the benchmarks named by the plan
    // -----------------------------------------------------------------------
    {
        std::set<std::string> names;
        for (auto& task : plan.tasks)
            for (auto& st : task.subtasks)
                if (!st.benchmark.empty()) names.insert(st.benchmark);

        if (!names.empty()) {
            std::cout << "Warming up " << names.size() << " benchmark(s), "
                      << bench::WARMUP_RUNS << " runs each:";
            for (const auto& n : names) {
                bench::prepare(n);
                std::cout << " " << n;
            }
            std::cout << "\n\n";
        }
    }

    // -----------------------------------------------------------------------
    //  9. Run — each source sleeps to its own next_fire_ns and re-arms by
    //  adding its exact period_ns, never a shared/truncated tick multiple.
    // -----------------------------------------------------------------------
    tm.start();

    uint64_t now = Dispatcher::monotonic_ns();
    for (auto& src : sources) src.next_fire_ns = now + src.period_ns;
    uint64_t end_ns = now + duration_ms * 1'000'000ULL;

    while (true) {
        auto next = std::min_element(sources.begin(), sources.end(),
            [](const SourceSched& a, const SourceSched& b) {
                return a.next_fire_ns < b.next_fire_ns;
            });
        if (next->next_fire_ns > end_ns) break;

        struct timespec ts;
        ts.tv_sec  = next->next_fire_ns / 1'000'000'000ULL;
        ts.tv_nsec = next->next_fire_ns % 1'000'000'000ULL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

        auto& tmet = task_mmap.at(next->task_id);
        {
            std::lock_guard<std::mutex> lk(tmet.mtx);
            tmet.pending_release_ns.push_back(next->next_fire_ns);
        }
        tm.notify(next->id);

        next->next_fire_ns += next->period_ns;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.stop();

    // -----------------------------------------------------------------------
    //  10. Export raw latency samples to CSV
    // -----------------------------------------------------------------------
    {
        std::ofstream csv("latency_samples.csv");
        csv << "subtask_id,period_ms,core,priority,latency_us\n";
        csv << std::fixed << std::setprecision(3);
        for (auto& [id, m] : mmap)
            for (auto lat : m.latency_ns)
                csv << m.id << ','
                    << (m.period_ns / 1'000'000ULL) << ','
                    << m.core << ','
                    << m.priority << ','
                    << (lat / 1000.0) << '\n';
        std::cout << "Raw samples written to latency_samples.csv\n\n";
    }

    // -----------------------------------------------------------------------
    //  11. Report — per-subtask latency (identical layout to evaluation.cpp)
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

    // -----------------------------------------------------------------------
    //  12. Report — end-to-end response time per task
    // -----------------------------------------------------------------------
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n=== End-to-end Response Time per Task ===\n";
    std::cout << std::left
              << std::setw(6) << "Task"
              << std::setw(6) << "Jobs"
              << std::setw(W) << "Resp_min(us)"
              << std::setw(W) << "Resp_mean(us)"
              << std::setw(W) << "Resp_max(us)"
              << "Misses\n"
              << std::string(6 + 6 + W * 3 + 6, '-') << "\n";

    for (auto& [task_id, tmet] : task_mmap) {
        auto& rv = tmet.response_ns;
        std::cout << std::left
                  << std::setw(6) << task_id
                  << std::setw(6) << static_cast<int>(rv.size())
                  << std::setw(W) << ns_to_us(static_cast<double>(vmin(rv)))
                  << std::setw(W) << ns_to_us(mean(rv))
                  << std::setw(W) << ns_to_us(static_cast<double>(vmax(rv)))
                  << tmet.deadline_misses << "\n";
    }

    return 0;
}
